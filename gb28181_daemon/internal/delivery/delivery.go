// Package delivery bridges durable media events to standard GB28181 Alarm
// messages. The receive and Alarm sink have independent durable state.
package delivery

import (
	"context"
	cryptorand "crypto/rand"
	"errors"
	"fmt"
	"log/slog"
	"math/big"
	"path/filepath"
	"strings"
	"time"

	"xiaoyu/gb28181-daemon/internal/gbxml"
	"xiaoyu/gb28181-daemon/internal/media"
	"xiaoyu/gb28181-daemon/internal/outbox"
)

// MessageSender is implemented by sipua.UA and keeps delivery independent of
// SIP construction and registration details.
type MessageSender interface {
	SendMessage(context.Context, []byte) error
}

type Config struct {
	DeviceID          string
	OutboxPath        string
	MaxRecords        int
	MaxBytes          int64
	AlarmPriority     int
	AlarmMethod       int
	AlarmTypes        map[string]int
	SendUpdates       bool
	SendEnds          bool
	MaxAttempts       int
	RetryBase         time.Duration
	RetryMax          time.Duration
	SendTimeout       time.Duration
	EvidenceEnabled   bool
	EvidenceDir       string
	EvidenceOutboxDir string
	EvidenceMaxBytes  int64
	EvidenceURL       string
	EvidenceToken     string
	EvidenceUploader  EvidenceUploader
}

type Service struct {
	cfg        Config
	subscriber media.EventSubscriber
	sender     MessageSender
	evidence   EvidenceUploader
	store      *outbox.Store
	log        *slog.Logger
}

var ErrReplayGap = errors.New("media event replay gap")

func New(cfg Config, subscriber media.EventSubscriber, sender MessageSender,
	log *slog.Logger) (*Service, error) {
	if subscriber == nil {
		return nil, errors.New("event subscriber is required")
	}
	if sender == nil {
		return nil, errors.New("message sender is required")
	}
	if strings.TrimSpace(cfg.DeviceID) == "" {
		return nil, errors.New("device id is required")
	}
	if log == nil {
		log = slog.Default()
	}
	if cfg.AlarmPriority <= 0 {
		cfg.AlarmPriority = 4
	}
	if cfg.AlarmMethod <= 0 {
		cfg.AlarmMethod = 5
	}
	if cfg.MaxAttempts <= 0 {
		cfg.MaxAttempts = 10
	}
	if cfg.RetryBase <= 0 {
		cfg.RetryBase = time.Second
	}
	if cfg.RetryMax <= 0 {
		cfg.RetryMax = 5 * time.Minute
	}
	if cfg.SendTimeout <= 0 {
		cfg.SendTimeout = 5 * time.Second
	}
	if cfg.EvidenceEnabled && strings.TrimSpace(cfg.EvidenceDir) == "" {
		return nil, errors.New("evidence directory is required when evidence is enabled")
	}
	if cfg.EvidenceEnabled && strings.TrimSpace(cfg.EvidenceOutboxDir) == "" {
		cfg.EvidenceOutboxDir = filepath.Join(filepath.Dir(cfg.OutboxPath), "evidence")
	}
	if cfg.EvidenceEnabled && cfg.EvidenceMaxBytes <= 0 {
		cfg.EvidenceMaxBytes = 64 << 20
	}
	evidence := cfg.EvidenceUploader
	if cfg.EvidenceEnabled && evidence == nil {
		var err error
		evidence, err = NewHTTPEvidenceUploader(cfg.EvidenceURL, cfg.EvidenceToken, nil)
		if err != nil {
			return nil, err
		}
	}
	alarmTypes := gbxml.DefaultAlarmTypes()
	for eventType, alarmType := range cfg.AlarmTypes {
		alarmTypes[eventType] = alarmType
	}
	cfg.AlarmTypes = alarmTypes
	store, err := outbox.Open(cfg.OutboxPath, outbox.Options{
		DeviceID:   cfg.DeviceID,
		MaxRecords: cfg.MaxRecords,
		MaxBytes:   cfg.MaxBytes,
	})
	if err != nil {
		return nil, err
	}
	if store.RecoveredCorruptTail() {
		log.Warn("delivery outbox recovered a corrupt final JSONL record",
			"path", cfg.OutboxPath)
	}
	if cfg.EvidenceEnabled {
		if err := cleanupOrphanedEvidence(cfg.EvidenceOutboxDir, store.Records()); err != nil {
			_ = store.Close()
			return nil, fmt.Errorf("clean evidence outbox: %w", err)
		}
	}
	return &Service{
		cfg:        cfg,
		subscriber: subscriber,
		sender:     sender,
		evidence:   evidence,
		store:      store,
		log:        log,
	}, nil
}

// Run reconnects the event stream from the durable ACK cursor. A received
// event is enqueued and synced before the producer cursor is acknowledged.
func (s *Service) Run(ctx context.Context) error {
	runCtx, cancel := context.WithCancel(ctx)
	wake := make(chan struct{}, 1)
	evidenceWake := make(chan struct{}, 1)
	alarmDone := make(chan struct{})
	evidenceDone := make(chan struct{})
	go func() {
		defer close(alarmDone)
		s.alarmLoop(runCtx, wake)
	}()
	go func() {
		defer close(evidenceDone)
		s.evidenceLoop(runCtx, evidenceWake)
	}()
	defer func() {
		cancel()
		<-alarmDone
		<-evidenceDone
		if err := s.store.Close(); err != nil {
			s.log.Error("close delivery outbox failed", "error", err)
		}
	}()

	signal(wake)
	signal(evidenceWake)
	for {
		afterCursor := s.store.LastAck()
		err := s.subscriber.ConsumeEvents(runCtx, afterCursor,
			func(info media.EventSubscribeInfo) error {
				if !info.ReplayGap {
					return nil
				}
				err := fmt.Errorf("%w: after_cursor=%d oldest_cursor=%d latest_cursor=%d",
					ErrReplayGap, afterCursor, info.OldestCursor, info.LatestCursor)
				s.log.Error("media event replay gap",
					"afterCursor", afterCursor,
					"oldestCursor", info.OldestCursor,
					"latestCursor", info.LatestCursor)
				return err
			}, func(notification media.EventNotification) error {
				return s.ingest(notification, wake, evidenceWake)
			})
		if runCtx.Err() != nil {
			return nil
		}
		if err != nil {
			if errors.Is(err, ErrReplayGap) || errors.Is(err, outbox.ErrCapacity) ||
				errors.Is(err, outbox.ErrInvalidEvent) {
				return err
			}
			s.log.Warn("media event subscription ended", "error", err)
		}
		if !wait(runCtx, s.cfg.RetryBase) {
			return nil
		}
	}
}

func (s *Service) ingest(notification media.EventNotification,
	wakes ...chan<- struct{}) error {
	alarmEnabled := s.alarmEnabled(notification.Event.Phase)
	evidenceEnabled := s.evidence != nil
	key := outbox.Key(s.cfg.DeviceID, notification.Event)
	existing, exists := s.store.Lookup(key)
	stageRequired := evidenceEnabled && notification.Event.Phase == media.EventPhaseStart &&
		notification.Event.EvidenceID != "" &&
		(!exists || (existing.EvidenceState != outbox.StateSent &&
			existing.EvidenceState != outbox.StateDead))
	stageErr := error(nil)
	if stageRequired {
		stageErr = stageEvidence(s.cfg.EvidenceDir, s.cfg.EvidenceOutboxDir,
			notification.Event, s.cfg.EvidenceMaxBytes)
		if stageErr != nil {
			s.log.Warn("stage analytics evidence failed; Alarm remains independent",
				"eventId", notification.Event.EventID,
				"evidenceId", notification.Event.EvidenceID, "error", stageErr)
		}
	}
	record, added, err := s.store.EnqueueWithEvidence(notification.Event,
		notification.Cursor, alarmEnabled, evidenceEnabled)
	if err != nil {
		if errors.Is(err, outbox.ErrUpdateDropped) {
			if ackErr := s.store.Ack(notification.Cursor); ackErr != nil {
				return fmt.Errorf("persist dropped UPDATE ACK cursor %d: %w", notification.Cursor, ackErr)
			}
			s.log.Warn("analytics UPDATE dropped under outbox pressure",
				"cursor", notification.Cursor,
				"droppedUpdates", s.store.DroppedUpdates())
			return nil
		}
		return fmt.Errorf("persist event cursor %d: %w", notification.Cursor, err)
	}
	if stageRequired {
		if stageErr == nil && !added &&
			(record.EvidenceState == outbox.StateDead ||
				record.EvidenceState == outbox.StateStaging ||
				record.EvidenceState == outbox.StateIgnored) {
			if err := s.store.MarkEvidenceState(record.Key, outbox.StatePending, nil); err != nil {
				return fmt.Errorf("requeue staged evidence for cursor %d: %w",
					notification.Cursor, err)
			}
		}
	}
	if stageErr != nil {
		attempt := record.EvidenceAttempts + 1
		terminal := attempt >= s.cfg.MaxAttempts
		updated, markErr := s.store.MarkEvidenceStagingFailure(record.Key,
			stageErr, terminal)
		if markErr != nil {
			return fmt.Errorf("persist evidence staging failure for cursor %d: %w",
				notification.Cursor, markErr)
		}
		if added && alarmEnabled && len(wakes) > 0 {
			// Alarm remains independently deliverable while the source event is
			// replayed until the evidence is durably staged.
			signal(wakes[0])
		}
		if terminal {
			if err := s.store.Ack(notification.Cursor); err != nil {
				return fmt.Errorf("persist terminal evidence ACK cursor %d: %w",
					notification.Cursor, err)
			}
			s.log.Error("analytics evidence staging moved to dead state",
				"key", updated.Key, "attempts", updated.EvidenceAttempts,
				"error", stageErr)
			return nil
		}
		return fmt.Errorf("stage analytics evidence for cursor %d: %w",
			notification.Cursor, stageErr)
	}
	if err := s.store.Ack(notification.Cursor); err != nil {
		return fmt.Errorf("persist event ACK cursor %d: %w", notification.Cursor, err)
	}
	if added && alarmEnabled {
		if len(wakes) > 0 {
			signal(wakes[0])
		}
	}
	if (added || stageErr == nil) && evidenceEnabled && notification.Event.Phase == media.EventPhaseStart &&
		notification.Event.EvidenceID != "" && len(wakes) > 1 {
		signal(wakes[1])
	}
	return nil
}

func (s *Service) alarmEnabled(phase string) bool {
	switch phase {
	case media.EventPhaseStart:
		return true
	case media.EventPhaseUpdate:
		return s.cfg.SendUpdates
	case media.EventPhaseEnd:
		return s.cfg.SendEnds
	default:
		return false
	}
}

func (s *Service) alarmLoop(ctx context.Context, wake <-chan struct{}) {
	ticker := time.NewTicker(s.cfg.RetryBase)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-wake:
			s.deliverPending(ctx)
		case <-ticker.C:
			s.deliverPending(ctx)
		}
	}
}

func (s *Service) evidenceLoop(ctx context.Context, wake <-chan struct{}) {
	if s.evidence == nil {
		return
	}
	ticker := time.NewTicker(s.cfg.RetryBase)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-wake:
			s.deliverPendingEvidence(ctx)
		case <-ticker.C:
			s.deliverPendingEvidence(ctx)
		}
	}
}

func (s *Service) deliverPending(ctx context.Context) {
	now := time.Now()
	for _, record := range s.store.PendingAlarms() {
		if record.AlarmNextRetryAtUnixNano > 0 &&
			now.UnixNano() < record.AlarmNextRetryAtUnixNano {
			continue
		}
		s.deliverOne(ctx, record)
		if ctx.Err() != nil {
			return
		}
	}
}

func (s *Service) deliverPendingEvidence(ctx context.Context) {
	now := time.Now()
	for _, record := range s.store.PendingEvidence() {
		if record.EvidenceNextRetryAtUnixNano > 0 &&
			now.UnixNano() < record.EvidenceNextRetryAtUnixNano {
			continue
		}
		s.deliverEvidenceOne(ctx, record)
		if ctx.Err() != nil {
			return
		}
	}
}

func (s *Service) deliverOne(ctx context.Context, record outbox.Record) {
	if record.AlarmSN == 0 || strings.TrimSpace(record.Event.ChannelID) == "" {
		_, err := s.store.MarkAlarmAttempt(record.Key,
			errors.New("alarm record is missing SN or channel_id"), true, time.Time{})
		if err != nil {
			s.log.Error("mark invalid alarm dead", "key", record.Key, "error", err)
		}
		return
	}
	description := describe(record.Event)
	body := gbxml.Alarm(gbxml.AlarmInput{
		SN:            record.AlarmSN,
		DeviceID:      record.Event.ChannelID,
		Priority:      s.cfg.AlarmPriority,
		Method:        s.cfg.AlarmMethod,
		EventTimeUS:   record.Event.EventTimeUS,
		AlarmType:     s.alarmType(record.Event.EventType),
		Description:   description,
		AlarmTypeInfo: record.Event.EventType,
	})
	sendCtx, cancel := context.WithTimeout(ctx, s.cfg.SendTimeout)
	err := s.sender.SendMessage(sendCtx, []byte(body))
	cancel()
	if err == nil {
		if markErr := s.store.MarkAlarmSent(record.Key); markErr != nil {
			s.log.Error("mark alarm sent failed", "key", record.Key, "error", markErr)
			return
		}
		s.log.Info("analytics alarm delivered", "key", record.Key,
			"cursor", record.Cursor, "sn", record.AlarmSN)
		return
	}
	attempt := record.AlarmAttempts + 1
	terminal := attempt >= s.cfg.MaxAttempts
	retryAt := time.Time{}
	if !terminal {
		retryAt = time.Now().Add(boundedBackoff(s.cfg.RetryBase, s.cfg.RetryMax, attempt))
	}
	updated, markErr := s.store.MarkAlarmAttempt(record.Key, err, terminal, retryAt)
	if markErr != nil {
		s.log.Error("record alarm delivery failure failed", "key", record.Key,
			"error", markErr)
		return
	}
	if terminal {
		s.log.Error("analytics alarm moved to dead state", "key", record.Key,
			"attempts", updated.AlarmAttempts, "error", err)
		return
	}
	s.log.Warn("analytics alarm delivery failed", "key", record.Key,
		"attempts", updated.AlarmAttempts, "retryAt", updated.AlarmNextRetryAtUnixNano,
		"error", err)
}

func (s *Service) deliverEvidenceOne(ctx context.Context, record outbox.Record) {
	metadata, image, err := loadEvidence(s.cfg.EvidenceOutboxDir, record.Event)
	if err == nil {
		sendCtx, cancel := context.WithTimeout(ctx, s.cfg.SendTimeout)
		err = s.evidence.Upload(sendCtx, record, metadata, image)
		cancel()
	}
	if err == nil {
		if markErr := s.store.MarkEvidenceSent(record.Key); markErr != nil {
			s.log.Error("mark evidence sent failed", "key", record.Key, "error", markErr)
			return
		}
		if cleanupErr := removeStagedEvidence(s.cfg.EvidenceOutboxDir, record.Event); cleanupErr != nil {
			s.log.Error("remove delivered evidence failed", "key", record.Key,
				"error", cleanupErr)
		}
		s.log.Info("analytics evidence delivered", "key", record.Key,
			"cursor", record.Cursor, "evidenceId", record.Event.EvidenceID)
		return
	}
	attempt := record.EvidenceAttempts + 1
	terminal := attempt >= s.cfg.MaxAttempts
	retryAt := time.Time{}
	if !terminal {
		retryAt = time.Now().Add(boundedBackoff(s.cfg.RetryBase, s.cfg.RetryMax, attempt))
	}
	updated, markErr := s.store.MarkEvidenceAttempt(record.Key, err, terminal, retryAt)
	if markErr != nil {
		s.log.Error("record evidence delivery failure failed", "key", record.Key,
			"error", markErr)
		return
	}
	if terminal {
		if cleanupErr := removeStagedEvidence(s.cfg.EvidenceOutboxDir, record.Event); cleanupErr != nil {
			s.log.Error("remove dead evidence failed", "key", record.Key,
				"error", cleanupErr)
		}
		s.log.Error("analytics evidence moved to dead state", "key", record.Key,
			"attempts", updated.EvidenceAttempts, "error", err)
		return
	}
	s.log.Warn("analytics evidence delivery failed", "key", record.Key,
		"attempts", updated.EvidenceAttempts,
		"retryAt", updated.EvidenceNextRetryAtUnixNano, "error", err)
}

func (s *Service) alarmType(eventType string) int {
	if alarmType, ok := s.cfg.AlarmTypes[eventType]; ok && alarmType > 0 {
		return alarmType
	}
	return 9
}

func boundedBackoff(base, maximum time.Duration, attempt int) time.Duration {
	if base <= 0 {
		base = time.Second
	}
	if maximum <= 0 {
		maximum = 5 * time.Minute
	}
	if maximum < base {
		maximum = base
	}
	if attempt < 1 {
		attempt = 1
	}
	delay := base
	for i := 1; i < attempt && delay < maximum; i++ {
		if delay > maximum/2 {
			delay = maximum
			break
		}
		delay *= 2
	}
	if delay > maximum {
		delay = maximum
	}
	// Keep at least half of the capped delay while adding full-range jitter
	// over the remaining half. This bounds reconnect bursts without allowing
	// an immediate retry storm.
	half := delay / 2
	if half <= 0 {
		return delay
	}
	jitter, err := cryptorand.Int(cryptorand.Reader, big.NewInt(int64(half)+1))
	if err != nil {
		return delay
	}
	return half + time.Duration(jitter.Int64())
}

func describe(event media.AnalyticsEvent) string {
	parts := []string{
		fmt.Sprintf("contract_version=%d", event.ContractVersion),
		"event_id=" + event.EventID,
		"phase=" + event.Phase,
		"reason=" + event.Reason,
		"clock_state=" + event.ClockState,
		fmt.Sprintf("event_seq=%d", event.EventSeq),
		fmt.Sprintf("frame_id=%d", event.FrameID),
		fmt.Sprintf("stream_epoch=%d", event.StreamEpoch),
		fmt.Sprintf("person_count=%d", event.PersonCount),
		fmt.Sprintf("delta_in=%d", event.DeltaIn),
		fmt.Sprintf("delta_out=%d", event.DeltaOut),
	}
	if event.SourcePTSValid {
		parts = append(parts, fmt.Sprintf("source_pts=%d", event.SourcePTS))
	}
	if event.EvidenceID != "" {
		parts = append(parts, "evidence_id="+event.EvidenceID)
	}
	if event.RuleID != "" {
		parts = append(parts, "rule_id="+event.RuleID)
	}
	return strings.Join(parts, ";")
}

func signal(wake chan<- struct{}) {
	select {
	case wake <- struct{}{}:
	default:
	}
}

func wait(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}
