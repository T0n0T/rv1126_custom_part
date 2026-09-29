package media

import (
	"context"
	"fmt"
	"log/slog"
	"strconv"
	"sync"
)

// Noop is a placeholder controller used before media_engine exists.
//
// With Simulate=false every call returns ErrUnavailable, so SIP flows fail
// cleanly instead of pretending a stream is running. With Simulate=true it
// logs operations and succeeds, which is useful to test the full SIP loop
// (REGISTER -> Catalog -> INVITE -> ACK -> BYE) without a media engine.
type Noop struct {
	Simulate bool
	Log      *slog.Logger
	mu       sync.Mutex
	active   Status
}

func (n *Noop) Ping(ctx context.Context) error {
	if !n.Simulate {
		return ErrUnavailable
	}
	n.Log.Info("media ping (simulated)")
	return nil
}

func (n *Noop) StartLive(ctx context.Context, target Target) error {
	if !n.Simulate {
		return ErrUnavailable
	}
	n.Log.Info("media start_live (simulated)",
		"session", target.SessionID, "channel", target.ChannelID,
		"dest", target.DestIP+":"+strconv.Itoa(target.DestPort), "ssrc", target.SSRC)
	n.mu.Lock()
	n.active = Status{Running: true, FPS: target.FPS, Bitrate: target.Bitrate,
		SessionID: target.SessionID, ChannelID: target.ChannelID, DestIP: target.DestIP,
		DestPort: target.DestPort, SSRC: target.SSRC}
	n.mu.Unlock()
	return nil
}

func (n *Noop) StopLive(ctx context.Context, sessionID string) error {
	if !n.Simulate {
		return ErrUnavailable
	}
	n.Log.Info("media stop_live (simulated)", "session", sessionID)
	n.mu.Lock()
	if !n.active.Running || n.active.SessionID != sessionID {
		n.mu.Unlock()
		return nil
	}
	n.active = Status{}
	n.mu.Unlock()
	return nil
}

func (n *Noop) KeepaliveLive(ctx context.Context, sessionID string) error {
	if !n.Simulate {
		return ErrUnavailable
	}
	n.mu.Lock()
	active := n.active.Running && n.active.SessionID == sessionID
	n.mu.Unlock()
	if active {
		n.Log.Debug("media keepalive_live (simulated)", "session", sessionID)
		return nil
	}
	return fmt.Errorf("media session not found: %s", sessionID)
}

func (n *Noop) Snapshot(ctx context.Context, channelID string) error {
	if !n.Simulate {
		return ErrUnavailable
	}
	n.Log.Info("media snapshot (simulated)", "channel", channelID)
	return nil
}

func (n *Noop) Status(ctx context.Context) (Status, error) {
	if !n.Simulate {
		return Status{}, ErrUnavailable
	}
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.active, nil
}
