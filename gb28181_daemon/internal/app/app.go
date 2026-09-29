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
	"sync"
	"time"

	"xiaoyu/gb28181-daemon/internal/config"
	"xiaoyu/gb28181-daemon/internal/delivery"
	"xiaoyu/gb28181-daemon/internal/gbxml"
	"xiaoyu/gb28181-daemon/internal/media"
	"xiaoyu/gb28181-daemon/internal/session"
	"xiaoyu/gb28181-daemon/internal/sipua"
	"xiaoyu/gb28181-daemon/internal/timesync"
)

const (
	registerAttempts      = 3
	keepaliveFailureLimit = 3
)

// Run starts the daemon and blocks until ctx is cancelled.
func Run(ctx context.Context, cfg *config.Config) error {
	log := newLogger(cfg)
	if cfg.TimeSync.Enabled {
		if err := syncClock(ctx, cfg, log); err != nil {
			log.Warn("startup clock sync failed; retrying in background", "error", err)
			go retryClock(ctx, cfg, log)
		}
	}
	ctrl := buildMediaController(cfg, log)
	sessions := session.NewManager()
	var mediaMu sync.Mutex
	if err := reconcileMedia(ctx, ctrl, cfg, log); err != nil {
		return err
	}

	handlers := sipua.Handlers{
		Query:   queryHandler(cfg),
		Control: controlHandler(cfg, log),
		PrepareInvite: func(info sipua.InviteInfo) error {
			mediaMu.Lock()
			defer mediaMu.Unlock()
			if !cfg.HasChannel(info.ChannelID) {
				return fmt.Errorf("unknown channel %q", info.ChannelID)
			}
			cctx, cancel := context.WithTimeout(ctx, cfg.MediaTimeout())
			defer cancel()
			status, err := ctrl.Status(cctx)
			if err != nil {
				return fmt.Errorf("media engine unavailable: %w", err)
			}
			if status.Running && status.SessionID != "" && sessions.Get(status.SessionID) == nil {
				log.Warn("releasing unowned media session before INVITE",
					"mediaSessionId", status.SessionID, "channel", status.ChannelID,
					"target", fmt.Sprintf("%s:%d", status.DestIP, status.DestPort),
					"ssrc", status.SSRC)
				if err := ctrl.StopLive(cctx, status.SessionID); err != nil {
					return fmt.Errorf("media orphan cleanup failed: %w", err)
				}
				status, err = ctrl.Status(cctx)
				if err != nil {
					return fmt.Errorf("media status after orphan cleanup: %w", err)
				}
			}
			if status.Running {
				return fmt.Errorf("%w: session=%s channel=%s target=%s:%d ssrc=%s", sipua.ErrBusy,
					status.SessionID, status.ChannelID, status.DestIP, status.DestPort, status.SSRC)
			}
			if !sessions.Reserve(info.CallID, session.Live{
				ChannelID: info.ChannelID,
				SSRC:      info.SSRC,
				DestIP:    info.DestIP,
				DestPort:  info.DestPort,
			}) {
				return fmt.Errorf("%w: another INVITE is pending", sipua.ErrBusy)
			}
			return nil
		},
		InviteAccepted: func(info sipua.InviteInfo) error {
			mediaMu.Lock()
			defer mediaMu.Unlock()
			sess := sessions.Get(info.CallID)
			if sess == nil || !sessions.IsPending(info.CallID) {
				return fmt.Errorf("INVITE session %s was cancelled before ACK", info.CallID)
			}
			target := media.Target{
				SessionID:   info.CallID,
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
				// An RPC timeout is ambiguous: the engine may have started RTP
				// but lost its reply. Stop by exact session ID before releasing
				// the reservation; retain it for retry when stop fails.
				stopCtx, stopCancel := context.WithTimeout(context.Background(), cfg.MediaTimeout())
				stopErr := ctrl.StopLive(stopCtx, info.CallID)
				stopCancel()
				if stopErr != nil {
					sessions.MarkCleanup(info.CallID)
					log.Error("media start failed and stop could not confirm cleanup",
						"callId", info.CallID, "channel", info.ChannelID,
						"target", fmt.Sprintf("%s:%d", info.DestIP, info.DestPort),
						"ssrc", info.SSRC, "startError", err, "stopError", stopErr)
				} else {
					sessions.Cancel(info.CallID)
				}
				return fmt.Errorf("media start failed: %w", err)
			}
			if sess = sessions.Activate(info.CallID); sess == nil {
				sessions.MarkCleanup(info.CallID)
				return fmt.Errorf("INVITE session %s could not activate after media start", info.CallID)
			}
			log.Info("live session started",
				"callId", info.CallID, "channel", info.ChannelID,
				"target", fmt.Sprintf("%s:%d", info.DestIP, info.DestPort),
				"ssrc", info.SSRC, "mediaSessionId", sess.CallID)
			return nil
		},
		InviteFailed: func(info sipua.InviteInfo, reason string) {
			mediaMu.Lock()
			defer mediaMu.Unlock()
			if sess := sessions.Cancel(info.CallID); sess != nil {
				log.Warn("INVITE session released before media start", "callId", info.CallID,
					"channel", info.ChannelID, "reason", reason)
			}
		},
		Bye: func(callID string) {
			mediaMu.Lock()
			defer mediaMu.Unlock()
			sess := sessions.Get(callID)
			if sess == nil {
				log.Info("BYE for already released session", "callId", callID)
			} else {
				log.Info("stopping live session from BYE", "callId", callID,
					"channel", sess.ChannelID,
					"target", fmt.Sprintf("%s:%d", sess.DestIP, sess.DestPort),
					"ssrc", sess.SSRC, "mediaSessionId", sess.CallID)
			}
			cctx, cancel := context.WithTimeout(context.Background(), cfg.MediaTimeout())
			defer cancel()
			if err := ctrl.StopLive(cctx, callID); err != nil {
				sessions.MarkCleanup(callID)
				log.Error("stop live failed", "callId", callID, "error", err)
				return
			}
			sessions.Stop(callID)
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
	if cfg.Events.Enabled {
		subscriber, ok := ctrl.(media.EventSubscriber)
		if !ok {
			return fmt.Errorf("events enabled but media controller has no event subscriber")
		}
		events, err := delivery.New(delivery.Config{
			DeviceID:          cfg.SIP.DeviceID,
			OutboxPath:        cfg.Events.OutboxPath,
			MaxRecords:        cfg.Events.MaxRecords,
			MaxBytes:          cfg.Events.MaxBytes,
			AlarmPriority:     cfg.Events.AlarmPriority,
			AlarmMethod:       cfg.Events.AlarmMethod,
			AlarmTypes:        cfg.Events.AlarmTypes,
			SendUpdates:       cfg.Events.SendUpdates,
			SendEnds:          cfg.Events.SendEnds,
			MaxAttempts:       cfg.Events.MaxAttempts,
			RetryBase:         time.Duration(cfg.Events.RetryBaseMS) * time.Millisecond,
			RetryMax:          time.Duration(cfg.Events.RetryMaxMS) * time.Millisecond,
			SendTimeout:       time.Duration(cfg.Events.SendTimeoutMS) * time.Millisecond,
			EvidenceEnabled:   cfg.Events.EvidenceEnabled,
			EvidenceDir:       cfg.Events.EvidenceDir,
			EvidenceOutboxDir: cfg.Events.EvidenceOutboxDir,
			EvidenceMaxBytes:  cfg.Events.EvidenceMaxBytes,
			EvidenceURL:       cfg.Events.EvidenceURL,
			EvidenceToken:     cfg.Events.EvidenceToken,
		}, subscriber, ua, log)
		if err != nil {
			return fmt.Errorf("open analytics delivery: %w", err)
		}
		go func() {
			if err := events.Run(ctx); err != nil && ctx.Err() == nil {
				log.Error("analytics delivery stopped", "error", err)
			}
		}()
	}
	go keepaliveLoop(ctx, ua, ctrl, sessions, &mediaMu, cfg, log)
	go mediaLeaseLoop(ctx, ctrl, sessions, &mediaMu, cfg, log)

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

func syncClock(ctx context.Context, cfg *config.Config, log *slog.Logger) error {
	timeout := time.Duration(cfg.TimeSync.TimeoutMS) * time.Millisecond
	if timeout <= 0 {
		timeout = 2 * time.Second
	}
	var ntpErr error
	if len(cfg.TimeSync.Servers) > 0 {
		ntpCtx, cancel := context.WithTimeout(ctx, timeout*time.Duration(len(cfg.TimeSync.Servers)))
		source, err := timesync.Sync(ntpCtx, cfg.TimeSync.Servers, timeout)
		cancel()
		if err == nil {
			log.Info("clock synchronized", "source", source,
				"time", time.Now().UTC().Format(time.RFC3339Nano))
			return nil
		}
		ntpErr = err
	}
	if len(cfg.TimeSync.HTTPServers) > 0 {
		httpCtx, cancel := context.WithTimeout(ctx, timeout*time.Duration(len(cfg.TimeSync.HTTPServers)))
		source, err := timesync.SyncHTTP(httpCtx, cfg.TimeSync.HTTPServers, timeout)
		cancel()
		if err == nil {
			log.Info("clock synchronized from HTTP Date", "source", source,
				"time", time.Now().UTC().Format(time.RFC3339Nano))
			return nil
		}
		if ntpErr != nil {
			return fmt.Errorf("NTP: %v; HTTP Date: %w", ntpErr, err)
		}
		return err
	}
	return ntpErr
}

func retryClock(ctx context.Context, cfg *config.Config, log *slog.Logger) {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			if err := syncClock(ctx, cfg, log); err == nil {
				return
			} else {
				log.Warn("clock sync retry failed", "error", err)
			}
		}
	}
}

// reconcileMedia is run before REGISTER. media_engine intentionally has a
// longer lifetime than the SIP daemon, so a daemon restart can otherwise
// leave an active RTP branch with no Call-ID owner. Stop that orphan through
// the normal RPC path and make the result visible in the log.
func reconcileMedia(ctx context.Context, ctrl media.Controller, cfg *config.Config, log *slog.Logger) error {
	cctx, cancel := context.WithTimeout(ctx, cfg.MediaTimeout())
	defer cancel()
	status, err := ctrl.Status(cctx)
	if err != nil {
		if cfg.Media.Mode == "rpc" {
			return fmt.Errorf("inspect media engine before SIP registration: %w", err)
		}
		return nil
	}
	if !status.Running {
		log.Info("media engine has no orphan live session")
		return nil
	}
	if status.SessionID == "" {
		return fmt.Errorf("media engine reports an active session without session_id")
	}
	log.Warn("releasing orphan live session before SIP registration",
		"sessionId", status.SessionID, "channel", status.ChannelID,
		"target", fmt.Sprintf("%s:%d", status.DestIP, status.DestPort),
		"ssrc", status.SSRC)
	stopCtx, stopCancel := context.WithTimeout(ctx, cfg.MediaTimeout())
	defer stopCancel()
	if err := ctrl.StopLive(stopCtx, status.SessionID); err != nil {
		return fmt.Errorf("release orphan media session %s: %w", status.SessionID, err)
	}
	log.Info("orphan live session released", "sessionId", status.SessionID)
	return nil
}

// mediaLeaseLoop keeps the media-engine lease alive only while the daemon
// still owns the Call-ID. If the daemon disappears, media_engine's own lease
// timeout stops the branch; if only the media process disappears, this loop
// drops the stale daemon mapping so the next INVITE can recover quickly.
func mediaLeaseLoop(ctx context.Context, ctrl media.Controller, sessions *session.Manager,
	mediaMu *sync.Mutex, cfg *config.Config, log *slog.Logger) {
	ticker := time.NewTicker(cfg.MediaLeaseRefresh())
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			mediaMu.Lock()
			for _, live := range sessions.All() {
				if sessions.NeedsCleanup(live.CallID) {
					stopOwnedMediaLocked(ctx, ctrl, sessions, cfg, log, "retry uncertain media cleanup")
					continue
				}
				if live.StartedAt.IsZero() {
					continue
				}
				if time.Since(live.StartedAt) >= cfg.MediaMaxLive() {
					stopOwnedMediaLocked(ctx, ctrl, sessions, cfg, log,
						"maximum live session duration reached without dialog refresh")
					continue
				}
				leaseCtx, cancel := context.WithTimeout(ctx, cfg.MediaTimeout())
				err := ctrl.KeepaliveLive(leaseCtx, live.CallID)
				cancel()
				if err == nil {
					continue
				}
				statusCtx, statusCancel := context.WithTimeout(ctx, cfg.MediaTimeout())
				status, statusErr := ctrl.Status(statusCtx)
				statusCancel()
				if statusErr == nil && (!status.Running || status.SessionID != live.CallID) {
					log.Warn("media session disappeared; releasing daemon mapping",
						"callId", live.CallID, "error", err)
					sessions.Stop(live.CallID)
					continue
				}
				log.Warn("media lease refresh failed", "callId", live.CallID, "error", err)
			}
			mediaMu.Unlock()
		}
	}
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

func keepaliveLoop(ctx context.Context, ua *sipua.UA, ctrl media.Controller,
	sessions *session.Manager, mediaMu *sync.Mutex, cfg *config.Config, log *slog.Logger) {
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
					mediaMu.Lock()
					stopOwnedMediaLocked(ctx, ctrl, sessions, cfg, log,
						"platform keepalive failure")
					mediaMu.Unlock()
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

// stopOwnedMedia is the recovery path for a platform restart or a network
// partition where BYE cannot arrive. It deliberately stops through the
// controller before deleting the daemon mapping, so a stop RPC failure can be
// retried by the normal lease/reconciliation paths.
func stopOwnedMediaLocked(ctx context.Context, ctrl media.Controller,
	sessions *session.Manager, cfg *config.Config, log *slog.Logger, reason string) {
	for _, live := range sessions.All() {
		stopCtx, cancel := context.WithTimeout(ctx, cfg.MediaTimeout())
		err := ctrl.StopLive(stopCtx, live.CallID)
		cancel()
		if err != nil {
			sessions.MarkCleanup(live.CallID)
			log.Error("recovery stop live failed", "callId", live.CallID,
				"channel", live.ChannelID, "reason", reason, "error", err)
			continue
		}
		sessions.Stop(live.CallID)
		log.Warn("live session released during signaling recovery",
			"callId", live.CallID, "channel", live.ChannelID, "reason", reason)
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
