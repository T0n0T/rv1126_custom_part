package media

import "context"

const (
	EventPhaseStart  = "START"
	EventPhaseUpdate = "UPDATE"
	EventPhaseEnd    = "END"
)

// Timebase describes the unit used by an event's source PTS.
type Timebase struct {
	Num uint32 `json:"num"`
	Den uint32 `json:"den"`
}

// AnalyticsEvent is the media_engine event contract carried over the local
// JSON-RPC notification channel. Source timing and the SIP message sequence
// are intentionally represented by different fields.
type AnalyticsEvent struct {
	ContractVersion       uint32   `json:"contract_version"`
	EventID               string   `json:"event_id"`
	ChannelID             string   `json:"channel_id"`
	StreamEpoch           uint64   `json:"stream_epoch"`
	EventType             string   `json:"event_type"`
	RuleID                string   `json:"rule_id"`
	Phase                 string   `json:"phase"`
	EventSeq              uint64   `json:"event_seq"`
	Reason                string   `json:"reason"`
	EventTimeUS           int64    `json:"event_time_us"`
	ClockState            string   `json:"clock_state"`
	SourcePTSValid        bool     `json:"source_pts_valid"`
	SourcePTS             int64    `json:"source_pts"`
	SourceTimebase        Timebase `json:"source_timebase"`
	FrameID               uint64   `json:"frame_id"`
	PersonCount           uint32   `json:"person_count"`
	DeltaIn               uint32   `json:"delta_in"`
	DeltaOut              uint32   `json:"delta_out"`
	ConfigVersion         uint64   `json:"config_version"`
	EvidenceID            string   `json:"evidence_id"`
	ResponsibleTrackCount uint32   `json:"responsible_track_count"`
	ResponsibleTrackIDs   []uint64 `json:"responsible_track_ids"`
}

// EventNotification is one journal record delivered by media_engine.
type EventNotification struct {
	Cursor uint64
	Event  AnalyticsEvent
}

// EventSubscribeInfo describes the journal view observed when subscribing.
type EventSubscribeInfo struct {
	ReplayGap    bool
	OldestCursor uint64
	LatestCursor uint64
}

// EventSubscriber is deliberately separate from Controller: control calls
// are short-lived, while event consumption owns a long-lived socket.
type EventSubscriber interface {
	ConsumeEvents(ctx context.Context, afterCursor uint64,
		onInfo func(EventSubscribeInfo) error,
		onEvent func(EventNotification) error) error
}
