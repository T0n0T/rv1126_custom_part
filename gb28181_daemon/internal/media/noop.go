package media

import (
	"context"
	"log/slog"
	"strconv"
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
	return nil
}

func (n *Noop) StopLive(ctx context.Context, sessionID string) error {
	if !n.Simulate {
		return ErrUnavailable
	}
	n.Log.Info("media stop_live (simulated)", "session", sessionID)
	return nil
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
	return Status{Running: true}, nil
}
