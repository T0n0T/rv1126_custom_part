// Package app wires the daemon together: config, SIP UA, GB XML handlers,
// media controller and session registry. It is the only package allowed to
// know about all of them.
package app

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"strings"
	"time"

	"xiaoyu/gb28181-daemon/internal/config"
	"xiaoyu/gb28181-daemon/internal/gbxml"
	"xiaoyu/gb28181-daemon/internal/media"
	"xiaoyu/gb28181-daemon/internal/session"
	"xiaoyu/gb28181-daemon/internal/sipua"
)

const (
	registerAttempts      = 3
	keepaliveFailureLimit = 3
)

// Run starts the daemon and blocks until ctx is cancelled.
func Run(ctx context.Context, cfg *config.Config) error {
	log := newLogger(cfg)
	ctrl := buildMediaController(cfg, log)
	sessions := session.NewManager()

	handlers := sipua.Handlers{
		Query:   queryHandler(cfg),
		Control: controlHandler(cfg, log),
		PrepareInvite: func(info sipua.InviteInfo) error {
			if !cfg.HasChannel(info.ChannelID) {
				return fmt.Errorf("unknown channel %q", info.ChannelID)
			}
			cctx, cancel := context.WithTimeout(ctx, cfg.MediaTimeout())
			defer cancel()
			if err := ctrl.Ping(cctx); err != nil {
				return fmt.Errorf("media engine unavailable: %w", err)
			}
			return nil
		},
		InviteAccepted: func(info sipua.InviteInfo) error {
			sess := sessions.Start(info.CallID, session.Live{
				ChannelID: info.ChannelID,
				SSRC:      info.SSRC,
				DestIP:    info.DestIP,
				DestPort:  info.DestPort,
			})
			target := media.Target{
				SessionID:   sess.CallID,
				ChannelID:   info.ChannelID,
				Codec:       cfg.Stream.Codec,
				Width:       cfg.Stream.Width,
				Height:      cfg.Stream.Height,
				FPS:         cfg.Stream.FPS,
				Bitrate:     cfg.Stream.Bitrate,
				DestIP:      info.DestIP,
				DestPort:    info.DestPort,
				SSRC:        info.SSRC,
				PayloadType: cfg.Stream.PayloadType,
			}
			cctx, cancel := context.WithTimeout(ctx, cfg.MediaTimeout())
			defer cancel()
			if err := ctrl.StartLive(cctx, target); err != nil {
				sessions.Stop(info.CallID)
				return err
			}
			log.Info("live session started",
				"callId", info.CallID, "channel", info.ChannelID,
				"target", fmt.Sprintf("%s:%d", info.DestIP, info.DestPort))
			return nil
		},
		Bye: func(callID string) {
			sess := sessions.Stop(callID)
			if sess == nil {
				log.Warn("BYE for unknown session", "callId", callID)
				return
			}
			cctx, cancel := context.WithTimeout(context.Background(), cfg.MediaTimeout())
			defer cancel()
			if err := ctrl.StopLive(cctx, sess.CallID); err != nil {
				log.Error("stop live failed", "callId", callID, "error", err)
				return
			}
			log.Info("live session stopped", "callId", callID)
		},
	}

	ua, err := sipua.New(sipua.Config{
		DeviceID:       cfg.SIP.DeviceID,
		LocalIP:        cfg.SIP.LocalIP,
		ServerAddr:     cfg.SIP.ServerAddr,
		PlatformID:     cfg.SIP.PlatformID,
		PlatformDomain: cfg.SIP.PlatformDomain,
		Password:       cfg.SIP.Password,
		Expires:        cfg.SIP.Expires,
	}, handlers, log)
	if err != nil {
		return err
	}
	defer ua.Close()

	if err := registerWithRetry(ctx, ua, log); err != nil {
		return err
	}
	go keepaliveLoop(ctx, ua, cfg, log)

	log.Info("gb28181 daemon running")
	<-ctx.Done()
	log.Info("shutting down")

	for _, live := range sessions.All() {
		cctx, cancel := context.WithTimeout(context.Background(), cfg.MediaTimeout())
		if err := ctrl.StopLive(cctx, live.CallID); err != nil {
			log.Error("shutdown stop live failed", "callId", live.CallID, "error", err)
		}
		cancel()
	}
	return nil
}

func queryHandler(cfg *config.Config) func(cmd, sn string) ([]byte, error) {
	channels := toGBChannels(cfg.Channels)
	return func(cmd, sn string) ([]byte, error) {
		switch cmd {
		case "Catalog":
			return []byte(gbxml.CatalogResponse(sn, cfg.SIP.DeviceID, channels)), nil
		case "DeviceInfo":
			return []byte(gbxml.DeviceInfoResponse(sn, cfg.SIP.DeviceID)), nil
		default:
			return nil, fmt.Errorf("unsupported CmdType %q", cmd)
		}
	}
}

// controlHandler is a test-only implementation of device-side IO control: it
// parses inbound <Control> MESSAGEs and prints the triggered IO to stdout
// instead of touching real GPIO. Auxiliary-switch commands (PTZCmd 8CH/8DH)
// are answered by the 200 OK alone; Record/Guard/Alarm get a proper
// <Response> body with Result.
func controlHandler(cfg *config.Config, log *slog.Logger) func(cmd, sn string, body []byte) ([]byte, error) {
	return func(cmd, sn string, body []byte) ([]byte, error) {
		ctrl, err := gbxml.ParseControl(body)
		if err != nil {
			return nil, fmt.Errorf("parse control: %w", err)
		}
		if num, action, ok := gbxml.DecodePTZAux(ctrl.PTZCmd); ok {
			fmt.Printf("[gb28181-io-test] %s IO%d %s (sn=%s)\n",
				time.Now().Format("2006-01-02 15:04:05.000"), num, action, sn)
			return nil, nil
		}
		switch {
		case ctrl.GuardCmd != "":
			fmt.Printf("[gb28181-io-test] %s guard %s (sn=%s)\n",
				time.Now().Format("2006-01-02 15:04:05.000"), ctrl.GuardCmd, sn)
		case ctrl.AlarmCmd != "":
			fmt.Printf("[gb28181-io-test] %s alarm %s (sn=%s)\n",
				time.Now().Format("2006-01-02 15:04:05.000"), ctrl.AlarmCmd, sn)
		case ctrl.RecordCmd != "":
			fmt.Printf("[gb28181-io-test] %s record %s (sn=%s)\n",
				time.Now().Format("2006-01-02 15:04:05.000"), ctrl.RecordCmd, sn)
		default:
			log.Info("device control received (test build)", "cmd", ctrl.CmdType,
				"sn", sn, "ptz", ctrl.PTZCmd, "teleBoot", ctrl.TeleBoot,
				"iFrame", ctrl.IFrameCmd)
			return nil, nil
		}
		return []byte(gbxml.ControlResponse(sn, ctrl.DeviceID, "OK")), nil
	}
}

func registerWithRetry(ctx context.Context, ua *sipua.UA, log *slog.Logger) error {
	var lastErr error
	for attempt := 1; attempt <= registerAttempts; attempt++ {
		regCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		err := ua.Register(regCtx)
		cancel()
		if err == nil {
			return nil
		}
		lastErr = err
		log.Warn("register attempt failed", "attempt", attempt, "error", err)
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(time.Duration(attempt) * 2 * time.Second):
		}
	}
	return fmt.Errorf("register failed after %d attempts: %w", registerAttempts, lastErr)
}

func keepaliveLoop(ctx context.Context, ua *sipua.UA, cfg *config.Config, log *slog.Logger) {
	interval := time.Duration(cfg.SIP.KeepaliveInterval) * time.Second
	if interval <= 0 {
		interval = 60 * time.Second
	}
	refresh := time.Duration(cfg.SIP.RegisterRefresh) * time.Second
	if refresh <= 0 {
		refresh = 5 * interval
	}

	keepaliveTicker := time.NewTicker(interval)
	defer keepaliveTicker.Stop()
	refreshTicker := time.NewTicker(refresh)
	defer refreshTicker.Stop()

	failures := 0
	for {
		select {
		case <-ctx.Done():
			return
		case <-keepaliveTicker.C:
			msgCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
			err := ua.SendMessage(msgCtx, []byte(gbxml.Keepalive(cfg.SIP.DeviceID)))
			cancel()
			if err != nil {
				failures++
				log.Warn("keepalive failed", "count", failures, "error", err)
				if failures >= keepaliveFailureLimit {
					failures = 0
					regCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
					err := ua.Register(regCtx)
					cancel()
					if err != nil {
						log.Error("re-register after keepalive failure failed", "error", err)
					} else {
						log.Info("re-registered after keepalive failure")
					}
				}
			} else {
				failures = 0
			}
		case <-refreshTicker.C:
			regCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
			err := ua.Register(regCtx)
			cancel()
			if err != nil {
				log.Error("periodic re-register failed", "error", err)
			}
		}
	}
}

func buildMediaController(cfg *config.Config, log *slog.Logger) media.Controller {
	switch cfg.Media.Mode {
	case "rpc":
		return media.NewRPC(cfg.Media.Socket, cfg.MediaTimeout(), log)
	default:
		return &media.Noop{Simulate: cfg.Media.Simulate, Log: log}
	}
}

func toGBChannels(in []config.Channel) []gbxml.Channel {
	out := make([]gbxml.Channel, 0, len(in))
	for _, ch := range in {
		out = append(out, gbxml.Channel{
			ID:      ch.ID,
			Name:    ch.Name,
			PTZType: ch.PTZType,
			Status:  ch.Status,
		})
	}
	return out
}

func newLogger(cfg *config.Config) *slog.Logger {
	var level slog.Level
	switch strings.ToLower(cfg.Log.Level) {
	case "debug":
		level = slog.LevelDebug
	case "warn":
		level = slog.LevelWarn
	case "error":
		level = slog.LevelError
	default:
		level = slog.LevelInfo
	}
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))
}
