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
	DeviceID      string
	OutboxPath    string
	MaxRecords    int
	MaxBytes      int64
	AlarmPriority int
	AlarmMethod   int
	AlarmTypes    map[string]int
	SendUpdates   bool
	SendEnds      bool
	MaxAttempts   int
	RetryBase     time.Duration
	RetryMax      time.Duration
	SendTimeout   time.Duration
}

type Service struct {
	cfg        Config
	subscriber media.EventSubscriber
	sender     MessageSender
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
	return &Service{
		cfg:        cfg,
		subscriber: subscriber,
		sender:     sender,
		store:      store,
		log:        log,
	}, nil
}

// Run reconnects the event stream from the durable ACK cursor. A received
// event is enqueued and synced before the producer cursor is acknowledged.
func (s *Service) Run(ctx context.Context) error {
	runCtx, cancel := context.WithCancel(ctx)
	wake := make(chan struct{}, 1)
	alarmDone := make(chan struct{})
	go func() {
		defer close(alarmDone)
		s.alarmLoop(runCtx, wake)
	}()
	defer func() {
		cancel()
		<-alarmDone
		if err := s.store.Close(); err != nil {
			s.log.Error("close delivery outbox failed", "error", err)
		}
	}()

	signal(wake)
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
				return s.ingest(notification, wake)
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
	wake chan<- struct{}) error {
	alarmEnabled := s.alarmEnabled(notification.Event.Phase)
	_, added, err := s.store.Enqueue(notification.Event, notification.Cursor,
		alarmEnabled)
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
	if err := s.store.Ack(notification.Cursor); err != nil {
		return fmt.Errorf("persist event ACK cursor %d: %w", notification.Cursor, err)
	}
	if added && alarmEnabled {
		signal(wake)
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
