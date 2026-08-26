package media

import "encoding/json"

// Wire types for the daemon <-> media_engine JSON protocol (v1).
// The contract is documented in custom_part/docs/ipc_architecture.md.

type rpcRequest struct {
	V      int             `json:"v"`
	ID     uint64          `json:"id"`
	Method string          `json:"method"`
	Params json.RawMessage `json:"params,omitempty"`
}

type rpcResponse struct {
	V      int             `json:"v"`
	ID     uint64          `json:"id"`
	Result json.RawMessage `json:"result,omitempty"`
	Error  *rpcError       `json:"error,omitempty"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

type startLiveParams struct {
	SessionID   string `json:"session_id"`
	ChannelID   string `json:"channel_id"`
	Codec       string `json:"codec"`
	Width       int    `json:"width"`
	Height      int    `json:"height"`
	FPS         int    `json:"fps"`
	Bitrate     int    `json:"bitrate"`
	DestIP      string `json:"dest_ip"`
	DestPort    int    `json:"dest_port"`
	SSRC        string `json:"ssrc"`
	PayloadType int    `json:"payload_type"`
}

type stopLiveParams struct {
	SessionID string `json:"session_id"`
}

type snapshotParams struct {
	ChannelID string `json:"channel_id"`
}

type okResult struct {
	OK bool `json:"ok"`
}

type statusResult struct {
	Running bool `json:"running"`
	FPS     int  `json:"fps"`
	Bitrate int  `json:"bitrate"`
}
