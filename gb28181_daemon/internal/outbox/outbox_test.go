package outbox

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"xiaoyu/gb28181-daemon/internal/media"
)

func testEvent(phase string, seq uint64) media.AnalyticsEvent {
	return media.AnalyticsEvent{
		ContractVersion: 1,
		EventID:         "event-1",
		ChannelID:       "35020000001310000001",
		StreamEpoch:     7,
		EventType:       "occupancy",
		RuleID:          "people",
		Phase:           phase,
		EventSeq:        seq,
		EventTimeUS:     1760000000000000,
		FrameID:         42,
		PersonCount:     2,
		SourcePTSValid:  true,
		SourcePTS:       9000,
		SourceTimebase:  media.Timebase{Num: 1, Den: 90000},
	}
}

func TestStorePersistsAckAndIndependentSinkState(t *testing.T) {
	path := filepath.Join(t.TempDir(), "delivery.jsonl")
	store, err := Open(path, Options{DeviceID: "device-1"})
	if err != nil {
		t.Fatal(err)
	}
	start, added, err := store.Enqueue(testEvent("START", 1), 11, true)
	if err != nil || !added {
		t.Fatalf("enqueue START: added=%v err=%v", added, err)
	}
	if start.AlarmSN == 0 || start.AlarmState != StatePending {
		t.Fatalf("unexpected START state: %+v", start)
	}
	update, added, err := store.Enqueue(testEvent("UPDATE", 2), 12, false)
	if err != nil || !added {
		t.Fatalf("enqueue UPDATE: added=%v err=%v", added, err)
	}
	if update.AlarmState != StateIgnored || update.EvidenceState != StateDisabled {
		t.Fatalf("unexpected UPDATE state: %+v", update)
	}
	if err := store.Ack(12); err != nil {
		t.Fatal(err)
	}
	if _, err := store.MarkAlarmAttempt(start.Key, errors.New("temporary"), false, time.Time{}); err != nil {
		t.Fatal(err)
	}
	if err := store.MarkEvidenceState(start.Key, StatePending, errors.New("not enabled")); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	store, err = Open(path, Options{DeviceID: "device-1"})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if got := store.LastAck(); got != 12 {
		t.Fatalf("last ACK = %d, want 12", got)
	}
	records := store.Records()
	if len(records) != 2 {
		t.Fatalf("record count = %d, want 2", len(records))
	}
	if records[0].AlarmAttempts != 1 || records[0].AlarmState != StatePending {
		t.Fatalf("alarm state was not restored: %+v", records[0])
	}
	if records[0].EvidenceState != StatePending {
		t.Fatalf("evidence state was not restored: %+v", records[0])
	}
	if err := store.MarkAlarmSent(records[0].Key); err != nil {
		t.Fatal(err)
	}
	if got := store.PendingAlarms(); len(got) != 0 {
		t.Fatalf("pending alarms after sent = %d, want 0", len(got))
	}
}

func TestStorePersistsEvidenceRetryState(t *testing.T) {
	path := filepath.Join(t.TempDir(), "delivery.jsonl")
	store, err := Open(path, Options{DeviceID: "device-1"})
	if err != nil {
		t.Fatal(err)
	}
	event := testEvent(media.EventPhaseStart, 1)
	event.EvidenceID = "ev-1"
	record, added, err := store.EnqueueWithEvidence(event, 21, true, true)
	if err != nil || !added || record.EvidenceState != StatePending {
		t.Fatalf("enqueue evidence event: added=%v record=%+v err=%v", added, record, err)
	}
	retryAt := time.Now().Add(time.Minute).Truncate(time.Nanosecond)
	if _, err := store.MarkEvidenceAttempt(record.Key, errors.New("temporary"), false, retryAt); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path, Options{DeviceID: "device-1"})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	restored := store.Records()[0]
	if restored.EvidenceState != StatePending || restored.EvidenceAttempts != 1 ||
		restored.EvidenceNextRetryAtUnixNano != retryAt.UnixNano() ||
		restored.EvidenceError != "temporary" {
		t.Fatalf("evidence retry state was not restored: %+v", restored)
	}
	if err := store.MarkEvidenceSent(restored.Key); err != nil {
		t.Fatal(err)
	}
	if got := store.Records()[0].EvidenceState; got != StateSent {
		t.Fatalf("evidence state after success = %q, want %q", got, StateSent)
	}
}

func TestStoreDeduplicatesReplayByEventPhase(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "delivery.jsonl"), Options{DeviceID: "device-1"})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	event := testEvent("START", 1)
	first, added, err := store.Enqueue(event, 20, true)
	if err != nil || !added {
		t.Fatalf("first enqueue: added=%v err=%v", added, err)
	}
	second, added, err := store.Enqueue(event, 99, true)
	if err != nil || added {
		t.Fatalf("replay enqueue: added=%v err=%v", added, err)
	}
	if second.Key != first.Key || second.Cursor != first.Cursor || second.AlarmSN != first.AlarmSN {
		t.Fatalf("replay changed idempotent record: first=%+v second=%+v", first, second)
	}
}

func TestStoreRejectsCorruptJournal(t *testing.T) {
	path := filepath.Join(t.TempDir(), "delivery.jsonl")
	if err := os.WriteFile(path, []byte(`{"v":1,"op":"unknown"}`+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(path, Options{DeviceID: "device-1"}); err == nil {
		t.Fatal("Open accepted corrupt outbox")
	}
}

func TestStoreValidatesJournalVersionAndOnlyTruncatesCorruptTail(t *testing.T) {
	path := filepath.Join(t.TempDir(), "delivery.jsonl")
	if err := os.WriteFile(path, []byte(`{"v":2,"op":"ack","cursor":1}`+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(path, Options{DeviceID: "device-1"}); err == nil {
		t.Fatal("Open accepted unsupported journal version")
	}

	store, err := Open(path, Options{DeviceID: "device-1"})
	if err == nil {
		_ = store.Close()
	}
	// Replace the file with a valid journal, then append an incomplete final
	// record. Recovery must retain the complete prefix and remove only the tail.
	if err := os.WriteFile(path, nil, 0600); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path, Options{DeviceID: "device-1"})
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := store.Enqueue(testEvent(media.EventPhaseStart, 1), 1, true); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	file, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := file.WriteString(`{"v":1,"op":"event"`); err != nil {
		_ = file.Close()
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path, Options{DeviceID: "device-1"})
	if err != nil {
		t.Fatal(err)
	}
	if len(store.Records()) != 1 {
		t.Fatalf("tail recovery lost complete records: %+v", store.Records())
	}
	if !store.RecoveredCorruptTail() {
		t.Fatal("tail recovery was not exposed as a health warning")
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Count(string(data), `"op":"event"`) != 1 {
		t.Fatal("corrupt tail was not truncated")
	}

	validLine := strings.TrimSuffix(string(data), "\n")
	if validLine == "" {
		t.Fatal("expected a complete journal line")
	}
	if err := os.WriteFile(path, []byte("{not-json}\n"+validLine+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(path, Options{DeviceID: "device-1"}); err == nil {
		t.Fatal("Open silently recovered a newline-terminated corrupt line")
	}
}

func TestStoreKeyScopesDeviceAndChannelAndRejectsCursorFallback(t *testing.T) {
	event := testEvent(media.EventPhaseStart, 1)
	if Key("device-a", event) == Key("device-b", event) {
		t.Fatal("device identity is missing from idempotency key")
	}
	otherChannel := event
	otherChannel.ChannelID = "35020000001310000002"
	if Key("device-a", event) == Key("device-a", otherChannel) {
		t.Fatal("channel identity is missing from idempotency key")
	}
	store, err := Open(filepath.Join(t.TempDir(), "delivery.jsonl"), Options{DeviceID: "device-a"})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	missingID := event
	missingID.EventID = ""
	if _, _, err := store.Enqueue(missingID, 99, true); !errors.Is(err, ErrInvalidEvent) {
		t.Fatalf("missing event_id error = %v, want ErrInvalidEvent", err)
	}
}

func TestStoreProtectsBoundariesAndPersistsSNAndRetryTime(t *testing.T) {
	path := filepath.Join(t.TempDir(), "delivery.jsonl")
	opts := Options{DeviceID: "device-1", MaxRecords: 1, MaxBytes: 32 << 10}
	store, err := Open(path, opts)
	if err != nil {
		t.Fatal(err)
	}
	start, added, err := store.Enqueue(testEvent(media.EventPhaseStart, 1), 1, true)
	if err != nil || !added || start.AlarmSN != 1 {
		t.Fatalf("enqueue START: added=%v record=%+v err=%v", added, start, err)
	}
	if _, _, err := store.Enqueue(testEvent(media.EventPhaseUpdate, 2), 2, false); !errors.Is(err, ErrUpdateDropped) {
		t.Fatalf("UPDATE pressure error = %v, want ErrUpdateDropped", err)
	}
	if got := store.DroppedUpdates(); got != 1 {
		t.Fatalf("dropped updates = %d, want 1", got)
	}
	retryAt := time.Now().Add(time.Minute).Truncate(time.Nanosecond)
	if _, err := store.MarkAlarmAttempt(start.Key, errors.New("temporary"), false, retryAt); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	store, err = Open(path, opts)
	if err != nil {
		t.Fatal(err)
	}
	restored := store.Records()[0]
	if restored.AlarmNextRetryAtUnixNano != retryAt.UnixNano() {
		t.Fatalf("retry time = %d, want %d", restored.AlarmNextRetryAtUnixNano, retryAt.UnixNano())
	}
	if err := store.MarkAlarmSent(restored.Key); err != nil {
		t.Fatal(err)
	}
	end, added, err := store.Enqueue(testEvent(media.EventPhaseEnd, 3), 3, true)
	if err != nil || !added || end.AlarmSN != 2 {
		t.Fatalf("enqueue END after compaction: added=%v record=%+v err=%v", added, end, err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), `"op":"snapshot"`) {
		t.Fatalf("boundary compaction did not write snapshot: %s", data)
	}
	if info, err := os.Stat(path); err != nil {
		t.Fatal(err)
	} else if info.Size() > opts.MaxBytes {
		t.Fatalf("compacted outbox size = %d, exceeds %d", info.Size(), opts.MaxBytes)
	}
}

func TestStoreCoalescesTerminalUpdateBeforeDropping(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "delivery.jsonl"), Options{
		DeviceID:   "device-1",
		MaxRecords: 1,
		MaxBytes:   32 << 10,
	})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	first, added, err := store.Enqueue(testEvent(media.EventPhaseUpdate, 1), 1, false)
	if err != nil || !added {
		t.Fatalf("first UPDATE: added=%v err=%v", added, err)
	}
	second, added, err := store.Enqueue(testEvent(media.EventPhaseUpdate, 2), 2, false)
	if err != nil || !added {
		t.Fatalf("coalesced UPDATE: added=%v err=%v", added, err)
	}
	if second.Key == first.Key || second.Cursor != 2 {
		t.Fatalf("latest UPDATE did not replace the old one: first=%+v second=%+v", first, second)
	}
	if records := store.Records(); len(records) != 1 || records[0].Event.EventSeq != 2 {
		t.Fatalf("coalesced records = %+v", records)
	}
	if got := store.DroppedUpdates(); got != 0 {
		t.Fatalf("dropped updates = %d, want 0 after coalescing", got)
	}
}
