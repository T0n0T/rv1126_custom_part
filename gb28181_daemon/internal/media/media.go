// Package media is the boundary between the daemon and media_engine.
// The daemon only depends on the Controller interface; concrete
// implementations (RPC over unix socket, Noop) live in this package.
package media

import (
	"context"
	"errors"
)

// ErrUnavailable is returned when no media engine is reachable.
var ErrUnavailable = errors.New("media engine unavailable")

// Target describes one live stream the media engine should start pushing.
type Target struct {
	SessionID   string
	ChannelID   string
	Codec       string
	Width       int
	Height      int
	FPS         int
	Bitrate     int
	DestIP      string
	DestPort    int
	SSRC        string
	PayloadType int
}

// Status is a snapshot of media engine state.
type Status struct {
	Running bool `json:"running"`
	FPS     int  `json:"fps"`
	Bitrate int  `json:"bitrate"`
}

// Controller is the media plane interface implemented by the unix-socket
// RPC client and (for development) the Noop simulator.
type Controller interface {
	Ping(ctx context.Context) error
	StartLive(ctx context.Context, target Target) error
	StopLive(ctx context.Context, sessionID string) error
	Snapshot(ctx context.Context, channelID string) error
	Status(ctx context.Context) (Status, error)
}
