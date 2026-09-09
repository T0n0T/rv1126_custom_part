package delivery

import (
	"context"
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"xiaoyu/gb28181-daemon/internal/media"
	"xiaoyu/gb28181-daemon/internal/outbox"
)

type fakeSubscriber struct{}

func (fakeSubscriber) ConsumeEvents(context.Context, uint64,
	func(media.EventSubscribeInfo) error, func(media.EventNotification) error) error {
	return errors.New("not used")
}

type fakeSender struct {
	bodies []string
	errors []error
}

func (s *fakeSender) SendMessage(_ context.Context, body []byte) error {
	s.bodies = append(s.bodies, string(body))
	if len(s.errors) == 0 {
		return nil
	}
	err := s.errors[0]
	s.errors = s.errors[1:]
	return err
}

type replayGapSubscriber struct{}

func (replayGapSubscriber) ConsumeEvents(_ context.Context, _ uint64,
	onInfo func(media.EventSubscribeInfo) error,
	_ func(media.EventNotification) error) error {
	return onInfo(media.EventSubscribeInfo{
		ReplayGap:    true,
		OldestCursor: 8,
		LatestCursor: 12,
	})
}

func deliveryEvent(phase string) media.AnalyticsEvent {
	return media.AnalyticsEvent{
		ContractVersion: 1,
		EventID:        "event-1",
		ChannelID:      "35020000001310000001",
		EventType:      "occupancy",
		Phase:          phase,
		Reason:         "confirmed",
		ClockState:     "synced",
		EventSeq:       88,
		EventTimeUS:    1760000000000000,
		FrameID:        12345,
		SourcePTSValid: true,
		SourcePTS:      9000,
	}
}

func newTestService(t *testing.T, sender MessageSender) *Service {
	t.Helper()
	service, err := New(Config{
		DeviceID:    "35020000001320000001",
		OutboxPath:  filepath.Join(t.TempDir(), "delivery.jsonl"),
		MaxAttempts: 3,
		RetryBase:   time.Millisecond,
		SendTimeout: time.Second,
	}, fakeSubscriber{}, sender, nil)
	if err != nil {
		t.Fatal(err)
	}
	return service
}

func TestIngestPersistsBeforeAcknowledging(t *testing.T) {
	service := newTestService(t, &fakeSender{})
	wake := make(chan struct{}, 1)
	if err := service.ingest(media.EventNotification{
		Cursor: 7,
		Event:  deliveryEvent("START"),
	}, wake); err != nil {
		t.Fatal(err)
	}
	defer service.store.Close()
	if got := service.store.LastAck(); got != 7 {
		t.Fatalf("last ACK = %d, want 7", got)
	}
	records := service.store.PendingAlarms()
	if len(records) != 1 || records[0].AlarmSN == 0 {
		t.Fatalf("unexpected pending records: %+v", records)
	}
}

func TestAlarmRetryKeepsStableSNAndSeparatesFrameFields(t *testing.T) {
	sender := &fakeSender{errors: []error{errors.New("temporary"), nil}}
	service := newTestService(t, sender)
	defer service.store.Close()
	wake := make(chan struct{}, 1)
	if err := service.ingest(media.EventNotification{
		Cursor: 8,
		Event:  deliveryEvent("START"),
	}, wake); err != nil {
		t.Fatal(err)
	}
	record := service.store.PendingAlarms()[0]
	service.deliverOne(context.Background(), record)
	service.deliverOne(context.Background(), service.store.PendingAlarms()[0])
	if len(sender.bodies) != 2 {
		t.Fatalf("send count = %d, want 2", len(sender.bodies))
	}
	if !strings.Contains(sender.bodies[0], "<SN>1</SN>") ||
		!strings.Contains(sender.bodies[1], "<SN>1</SN>") {
		t.Fatalf("retry changed SIP SN: %q / %q", sender.bodies[0], sender.bodies[1])
	}
	if strings.Contains(sender.bodies[0], "<SN>12345</SN>") ||
		!strings.Contains(sender.bodies[0], "frame_id=12345") ||
		!strings.Contains(sender.bodies[0], "event_seq=88") ||
		!strings.Contains(sender.bodies[0], "reason=confirmed") ||
		!strings.Contains(sender.bodies[0], "clock_state=synced") ||
		strings.Contains(sender.bodies[0], "source_pts=9000</SN>") {
		t.Fatalf("SN/frame/PTS fields were conflated: %q", sender.bodies[0])
	}
	if pending := service.store.PendingAlarms(); len(pending) != 0 {
		t.Fatalf("pending alarms after success = %d", len(pending))
	}
	if got := service.store.Records()[0].AlarmState; got != outbox.StateSent {
		t.Fatalf("alarm state = %q, want sent", got)
	}
}

func TestUpdatesAreIgnoredByDefault(t *testing.T) {
	service := newTestService(t, &fakeSender{})
	defer service.store.Close()
	wake := make(chan struct{}, 1)
	if err := service.ingest(media.EventNotification{
		Cursor: 9,
		Event:  deliveryEvent("UPDATE"),
	}, wake); err != nil {
		t.Fatal(err)
	}
	if pending := service.store.PendingAlarms(); len(pending) != 0 {
		t.Fatalf("default UPDATE unexpectedly entered Alarm sink: %d", len(pending))
	}
	if got := service.store.Records()[0].AlarmState; got != outbox.StateIgnored {
		t.Fatalf("UPDATE alarm state = %q, want ignored", got)
	}
}

func TestAlarmMappingCanBeOverridden(t *testing.T) {
	sender := &fakeSender{}
	service := newTestService(t, sender)
	defer service.store.Close()
	service.cfg.AlarmTypes["occupancy"] = 42
	wake := make(chan struct{}, 1)
	if err := service.ingest(media.EventNotification{
		Cursor: 10,
		Event:  deliveryEvent(media.EventPhaseStart),
	}, wake); err != nil {
		t.Fatal(err)
	}
	service.deliverOne(context.Background(), service.store.PendingAlarms()[0])
	if len(sender.bodies) != 1 || !strings.Contains(sender.bodies[0], "<AlarmType>42</AlarmType>") {
		t.Fatalf("custom alarm mapping was not used: %q", sender.bodies)
	}
}

func TestReplayGapStopsDeliveryInsteadOfReconnecting(t *testing.T) {
	service, err := New(Config{
		DeviceID:   "35020000001320000001",
		OutboxPath: filepath.Join(t.TempDir(), "delivery.jsonl"),
	}, replayGapSubscriber{}, &fakeSender{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	err = service.Run(context.Background())
	if !errors.Is(err, ErrReplayGap) {
		t.Fatalf("Run error = %v, want ErrReplayGap", err)
	}
}

func TestBoundedBackoffUsesJitterWithinConfiguredLimit(t *testing.T) {
	base := 100 * time.Millisecond
	maximum := 350 * time.Millisecond
	for attempt := 1; attempt <= 8; attempt++ {
		delay := boundedBackoff(base, maximum, attempt)
		if delay < base/2 || delay > maximum {
			t.Fatalf("attempt %d backoff = %s, want [%s,%s]", attempt, delay, base/2, maximum)
		}
	}
}
