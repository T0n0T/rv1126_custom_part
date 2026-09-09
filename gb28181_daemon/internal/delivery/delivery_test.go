package delivery

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
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

type fakeEvidenceUploader struct {
	calls  int
	errors []error
	data   [][]byte
}

func (u *fakeEvidenceUploader) Upload(_ context.Context, _ outbox.Record,
	metadata, image []byte) error {
	u.calls++
	u.data = append(u.data, append(append([]byte(nil), metadata...), image...))
	if len(u.errors) == 0 {
		return nil
	}
	err := u.errors[0]
	u.errors = u.errors[1:]
	return err
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
		EventID:         "event-1",
		ChannelID:       "35020000001310000001",
		StreamEpoch:     7,
		EventType:       "occupancy",
		Phase:           phase,
		Reason:          "confirmed",
		ClockState:      "synced",
		EventSeq:        88,
		EventTimeUS:     1760000000000000,
		FrameID:         12345,
		SourcePTSValid:  true,
		SourcePTS:       9000,
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

func TestEvidenceIsStagedBeforeAckAndRetriesIndependently(t *testing.T) {
	sourceDir := t.TempDir()
	outboxDir := t.TempDir()
	image := []byte{0xff, 0xd8, 0x01, 0x02, 0xff, 0xd9}
	digest := sha256.Sum256(image)
	evidenceID := "ev-1"
	metadata, err := json.Marshal(map[string]any{
		"v":                   2,
		"evidence_id":         evidenceID,
		"event_id":            "event-1",
		"channel_id":          "35020000001310000001",
		"stream_epoch":        7,
		"frame_id":            12345,
		"source_pts":          9000,
		"source_pts_valid":    true,
		"source_timebase_num": 1,
		"source_timebase_den": 1000000000,
		"capture_time_us":     1760000000000000,
		"width":               640,
		"height":              360,
		"accuracy":            "exact",
		"frame_delta":         0,
		"pts_delta":           0,
		"created_at_us":       1760000000000001,
		"expires_at_us":       1760001000000000,
		"object_key":          evidenceID + ".jpg",
		"storage_state":       "ready",
		"delivery_state":      "awaiting_consumer",
		"bytes":               len(image),
		"sha256":              hex.EncodeToString(digest[:]),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(sourceDir, evidenceID+".jpg"), image, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(sourceDir, evidenceID+".json"), metadata, 0600); err != nil {
		t.Fatal(err)
	}
	uploader := &fakeEvidenceUploader{errors: []error{errors.New("temporary")}}
	service, err := New(Config{
		DeviceID:          "35020000001320000001",
		OutboxPath:        filepath.Join(t.TempDir(), "delivery.jsonl"),
		MaxAttempts:       3,
		RetryBase:         time.Millisecond,
		SendTimeout:       time.Second,
		EvidenceEnabled:   true,
		EvidenceDir:       sourceDir,
		EvidenceOutboxDir: outboxDir,
		EvidenceUploader:  uploader,
	}, fakeSubscriber{}, &fakeSender{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer service.store.Close()
	wake := make(chan struct{}, 1)
	evidenceWake := make(chan struct{}, 1)
	event := deliveryEvent(media.EventPhaseStart)
	event.EvidenceID = evidenceID
	if err := service.ingest(media.EventNotification{Cursor: 15, Event: event}, wake, evidenceWake); err != nil {
		t.Fatal(err)
	}
	if got := service.store.LastAck(); got != 15 {
		t.Fatalf("last ACK = %d, want 15", got)
	}
	if _, err := os.Stat(filepath.Join(outboxDir, evidenceID+".jpg")); err != nil {
		t.Fatalf("staged evidence missing before ACK: %v", err)
	}
	record := service.store.PendingEvidence()[0]
	service.deliverEvidenceOne(context.Background(), record)
	record = service.store.Records()[0]
	if uploader.calls != 1 || record.EvidenceState != outbox.StatePending ||
		record.EvidenceAttempts != 1 {
		t.Fatalf("failed evidence delivery state = %+v, calls=%d", record, uploader.calls)
	}
	service.deliverEvidenceOne(context.Background(), record)
	record = service.store.Records()[0]
	if uploader.calls != 2 || record.EvidenceState != outbox.StateSent {
		t.Fatalf("successful evidence delivery state = %+v, calls=%d", record, uploader.calls)
	}
	if _, err := os.Stat(filepath.Join(outboxDir, evidenceID+".jpg")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("delivered staged JPEG still exists, stat error=%v", err)
	}
}

func TestEvidenceStageFailureDoesNotAckUntilReplayCanStage(t *testing.T) {
	sourceDir := t.TempDir()
	outboxDir := t.TempDir()
	evidenceID := "ev-replay"
	event := deliveryEvent(media.EventPhaseStart)
	event.EvidenceID = evidenceID
	service, err := New(Config{
		DeviceID:          "35020000001320000001",
		OutboxPath:        filepath.Join(t.TempDir(), "delivery.jsonl"),
		EvidenceEnabled:   true,
		EvidenceDir:       sourceDir,
		EvidenceOutboxDir: outboxDir,
		EvidenceUploader:  &fakeEvidenceUploader{},
	}, fakeSubscriber{}, &fakeSender{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer service.store.Close()
	wake := make(chan struct{}, 1)
	evidenceWake := make(chan struct{}, 1)
	notification := media.EventNotification{Cursor: 19, Event: event}
	if err := service.ingest(notification, wake, evidenceWake); err == nil {
		t.Fatal("ingest unexpectedly succeeded without source evidence")
	}
	if got := service.store.LastAck(); got != 0 {
		t.Fatalf("last ACK after staging failure = %d, want 0", got)
	}
	if pending := service.store.PendingAlarms(); len(pending) != 1 {
		t.Fatalf("alarm was not durably retained during evidence retry: %d", len(pending))
	}
	if records := service.store.Records(); len(records) != 1 ||
		records[0].EvidenceState != outbox.StateStaging {
		t.Fatalf("staging failure state = %+v, want staging", records)
	}

	image := []byte{0xff, 0xd8, 0x01, 0x02, 0xff, 0xd9}
	digest := sha256.Sum256(image)
	metadata, err := json.Marshal(map[string]any{
		"v":                   2,
		"evidence_id":         evidenceID,
		"event_id":            event.EventID,
		"channel_id":          event.ChannelID,
		"stream_epoch":        event.StreamEpoch,
		"frame_id":            event.FrameID,
		"source_pts":          event.SourcePTS,
		"source_pts_valid":    true,
		"source_timebase_num": 1,
		"source_timebase_den": 1000000000,
		"capture_time_us":     event.EventTimeUS,
		"width":               640,
		"height":              360,
		"accuracy":            "exact",
		"frame_delta":         0,
		"pts_delta":           0,
		"created_at_us":       1760000000000001,
		"expires_at_us":       1760001000000000,
		"object_key":          evidenceID + ".jpg",
		"storage_state":       "ready",
		"delivery_state":      "awaiting_consumer",
		"bytes":               len(image),
		"sha256":              hex.EncodeToString(digest[:]),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(outboxDir, evidenceID+".jpg"), image, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(outboxDir, evidenceID+".json"), metadata, 0600); err != nil {
		t.Fatal(err)
	}
	if err := service.ingest(notification, wake, evidenceWake); err != nil {
		t.Fatal(err)
	}
	if got := service.store.LastAck(); got != notification.Cursor {
		t.Fatalf("last ACK after successful replay = %d, want %d", got, notification.Cursor)
	}
	if pending := service.store.PendingEvidence(); len(pending) != 1 {
		t.Fatalf("staged evidence was not requeued: %d", len(pending))
	}
}

func TestHTTPEvidenceUploaderSendsIdempotentMultipartRequest(t *testing.T) {
	image := []byte{0xff, 0xd8, 0xff, 0xd9}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.Header.Get("Idempotency-Key") != "device-key" ||
			r.Header.Get("Authorization") != "Bearer secret" {
			t.Errorf("unexpected evidence request: method=%s headers=%v", r.Method, r.Header)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if err := r.ParseMultipartForm(1 << 20); err != nil {
			t.Errorf("parse multipart form: %v", err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if r.FormValue("event_id") != "event-1" || r.FormValue("evidence_id") != "ev-1" ||
			r.FormValue("metadata") != `{"evidence_id":"ev-1"}` {
			t.Errorf("unexpected evidence fields: form=%v", r.MultipartForm.Value)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		file, _, err := r.FormFile("image")
		if err != nil {
			t.Errorf("read image part: %v", err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		defer file.Close()
		got, err := io.ReadAll(file)
		if err != nil || !bytes.Equal(got, image) {
			t.Errorf("unexpected image: %x err=%v", got, err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	defer server.Close()
	uploader, err := NewHTTPEvidenceUploader(server.URL, "secret", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	if err := uploader.Upload(context.Background(), outbox.Record{
		Key:   "device-key",
		Event: media.AnalyticsEvent{EventID: "event-1", EvidenceID: "ev-1"},
	}, []byte(`{"evidence_id":"ev-1"}`), image); err != nil {
		t.Fatal(err)
	}
}
