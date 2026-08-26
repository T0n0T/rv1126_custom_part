package media

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net"
	"sync/atomic"
	"time"
)

// RPC implements Controller over the unix-socket JSON protocol.
// Each call opens a short-lived connection: control traffic is low rate and
// this keeps the client stateless and trivially resilient to media_engine
// restarts.
type RPC struct {
	endpoint string
	timeout  time.Duration
	log      *slog.Logger
	nextID   atomic.Uint64
}

func NewRPC(endpoint string, timeout time.Duration, log *slog.Logger) *RPC {
	return &RPC{endpoint: endpoint, timeout: timeout, log: log}
}

// ProtocolError maps a media_engine error response to a Go error.
type ProtocolError struct {
	Code    int
	Message string
}

func (e *ProtocolError) Error() string {
	return fmt.Sprintf("media rpc error %d: %s", e.Code, e.Message)
}

func (r *RPC) Ping(ctx context.Context) error {
	var out okResult
	if err := r.call(ctx, "media.ping", nil, &out); err != nil {
		return err
	}
	if !out.OK {
		return fmt.Errorf("media ping returned ok=false")
	}
	return nil
}

func (r *RPC) StartLive(ctx context.Context, t Target) error {
	var out okResult
	params := startLiveParams{
		SessionID:   t.SessionID,
		ChannelID:   t.ChannelID,
		Codec:       t.Codec,
		Width:       t.Width,
		Height:      t.Height,
		FPS:         t.FPS,
		Bitrate:     t.Bitrate,
		DestIP:      t.DestIP,
		DestPort:    t.DestPort,
		SSRC:        t.SSRC,
		PayloadType: t.PayloadType,
	}
	if err := r.call(ctx, "media.start_live", params, &out); err != nil {
		return err
	}
	if !out.OK {
		return fmt.Errorf("media start_live returned ok=false")
	}
	return nil
}

func (r *RPC) StopLive(ctx context.Context, sessionID string) error {
	var out okResult
	if err := r.call(ctx, "media.stop_live", stopLiveParams{SessionID: sessionID}, &out); err != nil {
		return err
	}
	if !out.OK {
		return fmt.Errorf("media stop_live returned ok=false")
	}
	return nil
}

func (r *RPC) Snapshot(ctx context.Context, channelID string) error {
	var out okResult
	if err := r.call(ctx, "media.snapshot", snapshotParams{ChannelID: channelID}, &out); err != nil {
		return err
	}
	if !out.OK {
		return fmt.Errorf("media snapshot returned ok=false")
	}
	return nil
}

func (r *RPC) Status(ctx context.Context) (Status, error) {
	var out statusResult
	if err := r.call(ctx, "media.get_status", nil, &out); err != nil {
		return Status{}, err
	}
	return Status{Running: out.Running, FPS: out.FPS, Bitrate: out.Bitrate}, nil
}

func (r *RPC) call(ctx context.Context, method string, params any, out any) error {
	paramsRaw, err := json.Marshal(params)
	if err != nil {
		return fmt.Errorf("encode %s params: %w", method, err)
	}
	req := rpcRequest{V: 1, ID: r.nextID.Add(1), Method: method, Params: paramsRaw}

	conn, err := net.DialTimeout("unix", r.endpoint, r.timeout)
	if err != nil {
		return fmt.Errorf("dial media engine: %w", err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(r.timeout))

	data, err := json.Marshal(req)
	if err != nil {
		return err
	}
	if _, err := conn.Write(append(data, '\n')); err != nil {
		return fmt.Errorf("write to media engine: %w", err)
	}

	line, err := bufio.NewReader(conn).ReadBytes('\n')
	if err != nil {
		return fmt.Errorf("read from media engine: %w", err)
	}
	var resp rpcResponse
	if err := json.Unmarshal(line, &resp); err != nil {
		return fmt.Errorf("decode media response: %w", err)
	}
	if resp.ID != req.ID {
		return fmt.Errorf("media response id mismatch: got %d want %d", resp.ID, req.ID)
	}
	if resp.Error != nil {
		return &ProtocolError{Code: resp.Error.Code, Message: resp.Error.Message}
	}
	if out != nil && len(resp.Result) > 0 {
		if err := json.Unmarshal(resp.Result, out); err != nil {
			return fmt.Errorf("decode %s result: %w", method, err)
		}
	}
	return nil
}
