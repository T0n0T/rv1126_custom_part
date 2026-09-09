// Package outbox owns the daemon-side durable receive and sink state.
package outbox

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"xiaoyu/gb28181-daemon/internal/media"
)

const (
	JournalVersion    = 1
	DefaultMaxRecords = 4096
	DefaultMaxBytes   = int64(16 << 20)
	StatePending      = "pending"
	StateSent         = "sent"
	StateIgnored      = "ignored"
	StateDead         = "dead"
	StateDisabled     = "disabled"
	maxAlarmSN        = uint64(999999)
)

var (
	ErrClosed        = errors.New("delivery outbox is closed")
	ErrCapacity      = errors.New("delivery outbox capacity exhausted")
	ErrUpdateDropped = errors.New("delivery outbox dropped UPDATE under pressure")
	ErrInvalidEvent  = errors.New("invalid analytics event")
)

// Options controls the durable store. A zero limit selects the safe default.
type Options struct {
	DeviceID   string
	MaxRecords int
	MaxBytes   int64
}

// Record is one idempotent event phase and the independent sink states that
// must survive a daemon restart.
type Record struct {
	Key                      string
	Cursor                   uint64
	Event                    media.AnalyticsEvent
	AlarmState               string
	EvidenceState            string
	AlarmSN                  uint64
	AlarmAttempts            int
	AlarmLastError           string
	AlarmNextRetryAtUnixNano int64
	EvidenceError            string
}

type journalEntry struct {
	Version                  int                   `json:"v"`
	Op                       string                `json:"op"`
	DeviceID                 string                `json:"device_id,omitempty"`
	Key                      string                `json:"key,omitempty"`
	Cursor                   uint64                `json:"cursor,omitempty"`
	Event                    *media.AnalyticsEvent `json:"event,omitempty"`
	AlarmState               string                `json:"alarm_state,omitempty"`
	EvidenceState            string                `json:"evidence_state,omitempty"`
	AlarmSN                  uint64                `json:"alarm_sn,omitempty"`
	AlarmAttempts            int                   `json:"alarm_attempts,omitempty"`
	AlarmLastError           string                `json:"alarm_last_error,omitempty"`
	AlarmNextRetryAtUnixNano int64                 `json:"alarm_next_retry_at_unix_nano,omitempty"`
	EvidenceError            string                `json:"evidence_error,omitempty"`
	NextAlarmSN              uint64                `json:"next_alarm_sn,omitempty"`
	LastAck                  uint64                `json:"last_ack,omitempty"`
	DroppedUpdates           uint64                `json:"dropped_updates,omitempty"`
	Records                  []recordSnapshot      `json:"records,omitempty"`
}

type recordSnapshot struct {
	Key                      string               `json:"key"`
	Cursor                   uint64               `json:"cursor"`
	Event                    media.AnalyticsEvent `json:"event"`
	AlarmState               string               `json:"alarm_state"`
	EvidenceState            string               `json:"evidence_state"`
	AlarmSN                  uint64               `json:"alarm_sn"`
	AlarmAttempts            int                  `json:"alarm_attempts"`
	AlarmLastError           string               `json:"alarm_last_error,omitempty"`
	AlarmNextRetryAtUnixNano int64                `json:"alarm_next_retry_at_unix_nano,omitempty"`
	EvidenceError            string               `json:"evidence_error,omitempty"`
}

// Store is an append-only, versioned JSONL state log. Every mutation is
// synced before it becomes visible to callers, which makes the ACK ordering
// explicit: enqueue first, acknowledge the producer cursor second.
type Store struct {
	mu             sync.Mutex
	path           string
	file           *os.File
	deviceID       string
	maxRecords     int
	maxBytes       int64
	records        map[string]Record
	lastAck        uint64
	nextAlarmSN    uint64
	droppedUpdates uint64
	recoveredTail  bool
}

// Open opens or recovers a durable outbox with explicit limits and device
// identity used by the idempotency key.
func Open(path string, opts Options) (*Store, error) {
	if strings.TrimSpace(path) == "" {
		return nil, errors.New("outbox path is required")
	}
	if opts.MaxRecords < 0 || opts.MaxBytes < 0 {
		return nil, errors.New("outbox limits cannot be negative")
	}
	if opts.MaxRecords == 0 {
		opts.MaxRecords = DefaultMaxRecords
	}
	if opts.MaxBytes == 0 {
		opts.MaxBytes = DefaultMaxBytes
	}

	dir := filepath.Dir(path)
	if dir != "." && dir != string(filepath.Separator) {
		if err := os.MkdirAll(dir, 0700); err != nil {
			return nil, fmt.Errorf("create outbox directory: %w", err)
		}
	}

	store := &Store{
		path:        path,
		deviceID:    opts.DeviceID,
		maxRecords:  opts.MaxRecords,
		maxBytes:    opts.MaxBytes,
		records:     make(map[string]Record),
		nextAlarmSN: 1,
	}
	if err := store.load(); err != nil {
		return nil, err
	}
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	if err != nil {
		return nil, fmt.Errorf("open outbox %s: %w", path, err)
	}
	store.file = file
	if info, statErr := file.Stat(); statErr != nil {
		_ = file.Close()
		return nil, fmt.Errorf("stat outbox %s: %w", path, statErr)
	} else if info.Size() > store.maxBytes {
		if err := store.compactLocked(); err != nil {
			_ = store.Close()
			return nil, fmt.Errorf("compact outbox %s on open: %w", path, err)
		}
	}
	return store, nil
}

func (s *Store) load() error {
	file, err := os.Open(s.path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("read outbox %s: %w", s.path, err)
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return fmt.Errorf("stat outbox %s: %w", s.path, err)
	}

	reader := bufio.NewReader(file)
	var offset int64
	lineNumber := 0
	for {
		lineStart := offset
		line, readErr := reader.ReadBytes('\n')
		if len(line) == 0 && errors.Is(readErr, io.EOF) {
			return nil
		}
		lineNumber++
		offset += int64(len(line))
		if len(line) > int(8<<20) {
			return fmt.Errorf("decode outbox %s line %d: line exceeds 8 MiB", s.path, lineNumber)
		}
		isTail := offset == info.Size()
		payload := bytes.TrimSpace(line)
		if len(payload) == 0 {
			if isTail {
				if truncateErr := truncateTail(s.path, lineStart); truncateErr != nil {
					return fmt.Errorf("truncate empty outbox tail %s: %w", s.path, truncateErr)
				}
				s.recoveredTail = true
				return nil
			}
			return fmt.Errorf("decode outbox %s line %d: empty journal entry", s.path, lineNumber)
		}

		var entry journalEntry
		if err := json.Unmarshal(payload, &entry); err != nil {
			// A power loss can leave a partial final write. Only the final line is
			// recoverable; a malformed line with later bytes is fatal.
			if isTail {
				if truncateErr := truncateTail(s.path, lineStart); truncateErr != nil {
					return fmt.Errorf("truncate corrupt outbox tail %s: %w", s.path, truncateErr)
				}
				s.recoveredTail = true
				return nil
			}
			return fmt.Errorf("decode outbox %s line %d: %w", s.path, lineNumber, err)
		}
		if err := s.apply(entry); err != nil {
			return fmt.Errorf("apply outbox %s line %d: %w", s.path, lineNumber, err)
		}
		if errors.Is(readErr, io.EOF) {
			return nil
		}
		if readErr != nil {
			return fmt.Errorf("read outbox %s: %w", s.path, readErr)
		}
	}
}

func truncateTail(path string, offset int64) error {
	file, err := os.OpenFile(path, os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer file.Close()
	if err := file.Truncate(offset); err != nil {
		return err
	}
	return file.Sync()
}

func (s *Store) apply(entry journalEntry) error {
	if entry.Version != JournalVersion {
		return fmt.Errorf("unsupported journal version %d, want %d", entry.Version, JournalVersion)
	}
	if entry.DeviceID != "" && s.deviceID != "" && entry.DeviceID != s.deviceID {
		return fmt.Errorf("journal device_id %q does not match configured device_id %q", entry.DeviceID, s.deviceID)
	}
	switch entry.Op {
	case "snapshot":
		if entry.NextAlarmSN == 0 || entry.NextAlarmSN > maxAlarmSN {
			return fmt.Errorf("snapshot has invalid next_alarm_sn %d", entry.NextAlarmSN)
		}
		records := make(map[string]Record, len(entry.Records))
		for _, snapshot := range entry.Records {
			record, err := snapshot.record()
			if err != nil {
				return err
			}
			if s.deviceID != "" && record.Key != Key(s.deviceID, record.Event) {
				return fmt.Errorf("snapshot key does not match event identity for %q", record.Key)
			}
			if _, exists := records[record.Key]; exists {
				return fmt.Errorf("snapshot contains duplicate key %q", record.Key)
			}
			records[record.Key] = record
		}
		s.records = records
		s.lastAck = entry.LastAck
		s.nextAlarmSN = entry.NextAlarmSN
		s.droppedUpdates = entry.DroppedUpdates
	case "event":
		if entry.Key == "" || entry.Event == nil {
			return errors.New("event entry requires key and event")
		}
		if entry.NextAlarmSN == 0 || entry.NextAlarmSN > maxAlarmSN {
			return fmt.Errorf("event has invalid next_alarm_sn %d", entry.NextAlarmSN)
		}
		record := Record{
			Key:                      entry.Key,
			Cursor:                   entry.Cursor,
			Event:                    *entry.Event,
			AlarmState:               entry.AlarmState,
			EvidenceState:            entry.EvidenceState,
			AlarmSN:                  entry.AlarmSN,
			AlarmAttempts:            entry.AlarmAttempts,
			AlarmLastError:           entry.AlarmLastError,
			AlarmNextRetryAtUnixNano: entry.AlarmNextRetryAtUnixNano,
			EvidenceError:            entry.EvidenceError,
		}
		if err := validateRecord(record); err != nil {
			return err
		}
		if s.deviceID != "" && record.Key != Key(s.deviceID, record.Event) {
			return fmt.Errorf("event key does not match event identity for %q", record.Key)
		}
		s.records[entry.Key] = record
		s.nextAlarmSN = entry.NextAlarmSN
	case "ack":
		if entry.Cursor > s.lastAck {
			s.lastAck = entry.Cursor
		}
	case "alarm_state", "evidence_state":
		record, ok := s.records[entry.Key]
		if !ok {
			return fmt.Errorf("state entry references unknown key %q", entry.Key)
		}
		if entry.Op == "alarm_state" {
			record.AlarmState = entry.AlarmState
			record.AlarmAttempts = entry.AlarmAttempts
			record.AlarmLastError = entry.AlarmLastError
			record.AlarmNextRetryAtUnixNano = entry.AlarmNextRetryAtUnixNano
		} else {
			record.EvidenceState = entry.EvidenceState
			record.EvidenceError = entry.EvidenceError
		}
		if err := validateRecord(record); err != nil {
			return err
		}
		s.records[entry.Key] = record
	default:
		return fmt.Errorf("unknown operation %q", entry.Op)
	}
	return nil
}

func (snapshot recordSnapshot) record() (Record, error) {
	record := Record{
		Key:                      snapshot.Key,
		Cursor:                   snapshot.Cursor,
		Event:                    snapshot.Event,
		AlarmState:               snapshot.AlarmState,
		EvidenceState:            snapshot.EvidenceState,
		AlarmSN:                  snapshot.AlarmSN,
		AlarmAttempts:            snapshot.AlarmAttempts,
		AlarmLastError:           snapshot.AlarmLastError,
		AlarmNextRetryAtUnixNano: snapshot.AlarmNextRetryAtUnixNano,
		EvidenceError:            snapshot.EvidenceError,
	}
	if err := validateRecord(record); err != nil {
		return Record{}, err
	}
	return record, nil
}

func validateRecord(record Record) error {
	if record.Key == "" || record.Event.EventID == "" || record.Event.ChannelID == "" {
		return fmt.Errorf("%w: record key, event_id and channel_id are required", ErrInvalidEvent)
	}
	if record.Event.EventSeq == 0 {
		return fmt.Errorf("%w: event_seq must be positive", ErrInvalidEvent)
	}
	switch record.Event.Phase {
	case media.EventPhaseStart, media.EventPhaseUpdate, media.EventPhaseEnd:
	default:
		return fmt.Errorf("%w: unknown phase %q", ErrInvalidEvent, record.Event.Phase)
	}
	switch record.AlarmState {
	case StatePending:
		if record.AlarmSN == 0 {
			return errors.New("pending alarm record requires alarm SN")
		}
	case StateSent, StateIgnored, StateDead:
	default:
		return fmt.Errorf("unknown alarm state %q", record.AlarmState)
	}
	switch record.EvidenceState {
	case StatePending, StateSent, StateIgnored, StateDead, StateDisabled:
	default:
		return fmt.Errorf("unknown evidence state %q", record.EvidenceState)
	}
	if record.AlarmAttempts < 0 || record.AlarmNextRetryAtUnixNano < 0 {
		return errors.New("negative alarm retry state")
	}
	return nil
}

func (s *Store) appendLocked(entry journalEntry) error {
	if s.file == nil {
		return ErrClosed
	}
	data, err := encodeEntry(entry)
	if err != nil {
		return fmt.Errorf("encode outbox entry: %w", err)
	}
	if info, statErr := s.file.Stat(); statErr != nil {
		return fmt.Errorf("stat outbox before append: %w", statErr)
	} else if info.Size()+int64(len(data)) > s.maxBytes {
		if err := s.compactLocked(); err != nil {
			return err
		}
		info, statErr = s.file.Stat()
		if statErr != nil {
			return fmt.Errorf("stat compacted outbox: %w", statErr)
		}
		if info.Size()+int64(len(data)) > s.maxBytes {
			return fmt.Errorf("%w: append needs %d bytes with %d bytes already used", ErrCapacity, len(data), info.Size())
		}
	}
	if err := writeAll(s.file, data); err != nil {
		return fmt.Errorf("append outbox: %w", err)
	}
	if err := s.file.Sync(); err != nil {
		return fmt.Errorf("sync outbox: %w", err)
	}
	return nil
}

func encodeEntry(entry journalEntry) ([]byte, error) {
	entry.Version = JournalVersion
	data, err := json.Marshal(entry)
	if err != nil {
		return nil, err
	}
	return append(data, '\n'), nil
}

func writeAll(file *os.File, data []byte) error {
	for len(data) > 0 {
		n, err := file.Write(data)
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		data = data[n:]
	}
	return nil
}

func (s *Store) compactLocked() error {
	if s.file == nil {
		return ErrClosed
	}
	data, err := encodeEntry(journalEntry{
		Op:             "snapshot",
		DeviceID:       s.deviceID,
		LastAck:        s.lastAck,
		NextAlarmSN:    s.nextAlarmSN,
		DroppedUpdates: s.droppedUpdates,
		Records:        s.snapshotRecordsLocked(),
	})
	if err != nil {
		return fmt.Errorf("encode outbox snapshot: %w", err)
	}
	if int64(len(data)) > s.maxBytes {
		return fmt.Errorf("%w: snapshot needs %d bytes, limit is %d", ErrCapacity, len(data), s.maxBytes)
	}
	if err := s.file.Sync(); err != nil {
		return fmt.Errorf("sync outbox before compact: %w", err)
	}

	dir := filepath.Dir(s.path)
	temp, err := os.CreateTemp(dir, "."+filepath.Base(s.path)+".compact-*")
	if err != nil {
		return fmt.Errorf("create outbox compact file: %w", err)
	}
	tempName := temp.Name()
	defer os.Remove(tempName)
	if err := temp.Chmod(0600); err != nil {
		_ = temp.Close()
		return fmt.Errorf("chmod outbox compact file: %w", err)
	}
	if err := writeAll(temp, data); err != nil {
		_ = temp.Close()
		return fmt.Errorf("write outbox compact file: %w", err)
	}
	if err := temp.Sync(); err != nil {
		_ = temp.Close()
		return fmt.Errorf("sync outbox compact file: %w", err)
	}
	if err := temp.Close(); err != nil {
		return fmt.Errorf("close outbox compact file: %w", err)
	}
	if err := os.Rename(tempName, s.path); err != nil {
		return fmt.Errorf("replace outbox with compact file: %w", err)
	}

	oldFile := s.file
	if err := oldFile.Close(); err != nil {
		s.file = nil
		return fmt.Errorf("close old outbox after compact: %w", err)
	}
	newFile, err := os.OpenFile(s.path, os.O_WRONLY|os.O_APPEND, 0600)
	if err != nil {
		s.file = nil
		return fmt.Errorf("reopen compacted outbox: %w", err)
	}
	s.file = newFile
	// The file contents and rename are already durable. Directory fsync is a
	// best-effort durability enhancement; never leave the store writing to the
	// unlinked pre-compaction inode if a filesystem rejects directory sync.
	_ = syncDirectory(dir)
	return nil
}

func syncDirectory(path string) error {
	dir, err := os.Open(path)
	if err != nil {
		return err
	}
	defer dir.Close()
	return dir.Sync()
}

func (s *Store) snapshotRecordsLocked() []recordSnapshot {
	result := make([]recordSnapshot, 0, len(s.records))
	for _, record := range s.records {
		result = append(result, recordSnapshot{
			Key:                      record.Key,
			Cursor:                   record.Cursor,
			Event:                    record.Event,
			AlarmState:               record.AlarmState,
			EvidenceState:            record.EvidenceState,
			AlarmSN:                  record.AlarmSN,
			AlarmAttempts:            record.AlarmAttempts,
			AlarmLastError:           record.AlarmLastError,
			AlarmNextRetryAtUnixNano: record.AlarmNextRetryAtUnixNano,
			EvidenceError:            record.EvidenceError,
		})
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].Cursor == result[j].Cursor {
			return result[i].Key < result[j].Key
		}
		return result[i].Cursor < result[j].Cursor
	})
	return result
}

// Key returns the stable idempotency key for one device/channel event phase.
// Cursor is intentionally absent: it identifies delivery order, not the
// logical event, and must never become a substitute for event_id.
func Key(deviceID string, event media.AnalyticsEvent) string {
	encode := func(value string) string {
		return base64.RawURLEncoding.EncodeToString([]byte(value))
	}
	return strings.Join([]string{
		encode(deviceID),
		encode(event.ChannelID),
		encode(event.EventID),
		encode(event.Phase),
		strconv.FormatUint(event.EventSeq, 10),
	}, "|")
}

func validateEvent(deviceID string, event media.AnalyticsEvent) error {
	if strings.TrimSpace(deviceID) == "" {
		return fmt.Errorf("%w: device_id is required", ErrInvalidEvent)
	}
	if strings.TrimSpace(event.EventID) == "" {
		return fmt.Errorf("%w: event_id is required", ErrInvalidEvent)
	}
	if strings.TrimSpace(event.ChannelID) == "" {
		return fmt.Errorf("%w: channel_id is required", ErrInvalidEvent)
	}
	if event.EventSeq == 0 {
		return fmt.Errorf("%w: event_seq must be positive", ErrInvalidEvent)
	}
	switch event.Phase {
	case media.EventPhaseStart, media.EventPhaseUpdate, media.EventPhaseEnd:
	default:
		return fmt.Errorf("%w: unknown phase %q", ErrInvalidEvent, event.Phase)
	}
	return nil
}

// Enqueue durably records an event. alarmEnabled controls whether this phase
// enters the Alarm sink; evidence remains independently disabled for now.
func (s *Store) Enqueue(event media.AnalyticsEvent, cursor uint64,
	alarmEnabled bool) (Record, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.file == nil {
		return Record{}, false, ErrClosed
	}
	if err := validateEvent(s.deviceID, event); err != nil {
		return Record{}, false, err
	}
	key := Key(s.deviceID, event)
	if record, ok := s.records[key]; ok {
		return record, false, nil
	}
	if len(s.records) >= s.maxRecords {
		if event.Phase == media.EventPhaseUpdate {
			coalesced, err := s.coalesceUpdateLocked(event)
			if err != nil {
				return Record{}, false, err
			}
			if coalesced {
				// The compacted journal has made room for the latest UPDATE.
				// Continue with the normal durable append below.
			} else {
				s.droppedUpdates++
				return Record{}, false, ErrUpdateDropped
			}
		} else if err := s.makeRoomForBoundaryLocked(); err != nil {
			return Record{}, false, err
		}
	}

	alarmState := StateIgnored
	alarmSN := uint64(0)
	if alarmEnabled {
		alarmState = StatePending
		alarmSN = s.allocateAlarmSNLocked()
	}
	record := Record{
		Key:           key,
		Cursor:        cursor,
		Event:         event,
		AlarmState:    alarmState,
		EvidenceState: StateDisabled,
		AlarmSN:       alarmSN,
	}
	oldNext := s.nextAlarmSN
	entry := journalEntry{
		Op:            "event",
		DeviceID:      s.deviceID,
		Key:           record.Key,
		Cursor:        record.Cursor,
		Event:         &record.Event,
		AlarmState:    record.AlarmState,
		EvidenceState: record.EvidenceState,
		AlarmSN:       record.AlarmSN,
		NextAlarmSN:   s.nextAlarmSN,
	}
	appendErr := s.appendLocked(entry)
	if errors.Is(appendErr, ErrCapacity) && event.Phase != media.EventPhaseUpdate {
		appendErr = s.appendBoundaryAfterBytePressureLocked(entry)
	}
	if appendErr != nil {
		s.nextAlarmSN = oldNext
		if event.Phase == media.EventPhaseUpdate && errors.Is(appendErr, ErrCapacity) {
			s.droppedUpdates++
			return Record{}, false, ErrUpdateDropped
		}
		return Record{}, false, appendErr
	}
	s.records[key] = record
	return record, true, nil
}

func (s *Store) makeRoomForBoundaryLocked() error {
	backup := cloneRecords(s.records)
	for len(s.records) >= s.maxRecords {
		candidate, ok := oldestTerminal(s.records)
		if !ok {
			s.records = backup
			return fmt.Errorf("%w: START/END cannot be persisted without evicting a pending sink", ErrCapacity)
		}
		delete(s.records, candidate.Key)
	}
	if err := s.compactLocked(); err != nil {
		s.records = backup
		return err
	}
	return nil
}

func (s *Store) coalesceUpdateLocked(event media.AnalyticsEvent) (bool, error) {
	var candidate Record
	found := false
	for _, record := range s.records {
		if record.Event.ChannelID != event.ChannelID ||
			record.Event.EventID != event.EventID ||
			record.Event.Phase != media.EventPhaseUpdate ||
			record.AlarmState == StatePending ||
			record.EvidenceState == StatePending {
			continue
		}
		if !found || record.Cursor > candidate.Cursor {
			candidate = record
			found = true
		}
	}
	if !found {
		return false, nil
	}
	backup := cloneRecords(s.records)
	delete(s.records, candidate.Key)
	if err := s.compactLocked(); err != nil {
		s.records = backup
		return false, err
	}
	return true, nil
}

func (s *Store) appendBoundaryAfterBytePressureLocked(entry journalEntry) error {
	var lastErr error = ErrCapacity
	for {
		candidate, ok := oldestTerminal(s.records)
		if !ok {
			return lastErr
		}
		delete(s.records, candidate.Key)
		if err := s.compactLocked(); err != nil {
			return err
		}
		lastErr = s.appendLocked(entry)
		if lastErr == nil || !errors.Is(lastErr, ErrCapacity) {
			return lastErr
		}
	}
}

func cloneRecords(records map[string]Record) map[string]Record {
	clone := make(map[string]Record, len(records))
	for key, record := range records {
		clone[key] = record
	}
	return clone
}

func oldestTerminal(records map[string]Record) (Record, bool) {
	var candidate Record
	found := false
	for _, record := range records {
		if record.AlarmState == StatePending || record.EvidenceState == StatePending {
			continue
		}
		if !found || record.Cursor < candidate.Cursor ||
			(record.Cursor == candidate.Cursor && record.Key < candidate.Key) {
			candidate = record
			found = true
		}
	}
	return candidate, found
}

func (s *Store) allocateAlarmSNLocked() uint64 {
	if s.nextAlarmSN == 0 || s.nextAlarmSN > maxAlarmSN {
		s.nextAlarmSN = 1
	}
	sn := s.nextAlarmSN
	s.nextAlarmSN++
	if s.nextAlarmSN > maxAlarmSN {
		s.nextAlarmSN = 1
	}
	return sn
}

// Ack advances the durable producer cursor monotonically.
func (s *Store) Ack(cursor uint64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.file == nil {
		return ErrClosed
	}
	if cursor <= s.lastAck {
		return nil
	}
	if err := s.appendLocked(journalEntry{Op: "ack", Cursor: cursor}); err != nil {
		return err
	}
	s.lastAck = cursor
	return nil
}

func (s *Store) LastAck() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.lastAck
}

func (s *Store) MarkAlarmSent(key string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	record, ok := s.records[key]
	if !ok {
		return fmt.Errorf("unknown outbox key %q", key)
	}
	record.AlarmState = StateSent
	record.AlarmLastError = ""
	record.AlarmNextRetryAtUnixNano = 0
	if err := s.appendLocked(journalEntry{
		Op:                       "alarm_state",
		Key:                      key,
		AlarmState:               record.AlarmState,
		AlarmAttempts:            record.AlarmAttempts,
		AlarmNextRetryAtUnixNano: 0,
	}); err != nil {
		return err
	}
	s.records[key] = record
	return nil
}

// MarkAlarmAttempt persists the retry schedule along with the attempt. The
// optional time keeps callers that do not schedule another retry concise.
func (s *Store) MarkAlarmAttempt(key string, cause error, terminal bool,
	retryAt time.Time) (Record, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	record, ok := s.records[key]
	if !ok {
		return Record{}, fmt.Errorf("unknown outbox key %q", key)
	}
	record.AlarmAttempts++
	record.AlarmLastError = trimError(cause)
	record.AlarmNextRetryAtUnixNano = 0
	if terminal {
		record.AlarmState = StateDead
	} else {
		record.AlarmState = StatePending
		if !retryAt.IsZero() {
			record.AlarmNextRetryAtUnixNano = retryAt.UnixNano()
		}
	}
	if appendErr := s.appendLocked(journalEntry{
		Op:                       "alarm_state",
		Key:                      key,
		AlarmState:               record.AlarmState,
		AlarmAttempts:            record.AlarmAttempts,
		AlarmLastError:           record.AlarmLastError,
		AlarmNextRetryAtUnixNano: record.AlarmNextRetryAtUnixNano,
	}); appendErr != nil {
		return Record{}, appendErr
	}
	s.records[key] = record
	return record, nil
}

func (s *Store) MarkEvidenceState(key, state string, cause error) error {
	if state == "" {
		return errors.New("evidence state is required")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	record, ok := s.records[key]
	if !ok {
		return fmt.Errorf("unknown outbox key %q", key)
	}
	record.EvidenceState = state
	record.EvidenceError = trimError(cause)
	if appendErr := s.appendLocked(journalEntry{
		Op:            "evidence_state",
		Key:           key,
		EvidenceState: record.EvidenceState,
		EvidenceError: record.EvidenceError,
	}); appendErr != nil {
		return appendErr
	}
	s.records[key] = record
	return nil
}

func trimError(err error) string {
	if err == nil {
		return ""
	}
	message := err.Error()
	if len(message) > 512 {
		return message[:512]
	}
	return message
}

func (s *Store) PendingAlarms() []Record {
	s.mu.Lock()
	defer s.mu.Unlock()
	result := make([]Record, 0)
	for _, record := range s.records {
		if record.AlarmState == StatePending {
			result = append(result, record)
		}
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].Cursor == result[j].Cursor {
			return result[i].Key < result[j].Key
		}
		return result[i].Cursor < result[j].Cursor
	})
	return result
}

func (s *Store) Records() []Record {
	s.mu.Lock()
	defer s.mu.Unlock()
	result := make([]Record, 0, len(s.records))
	for _, record := range s.records {
		result = append(result, record)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].Cursor == result[j].Cursor {
			return result[i].Key < result[j].Key
		}
		return result[i].Cursor < result[j].Cursor
	})
	return result
}

func (s *Store) DroppedUpdates() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.droppedUpdates
}

// RecoveredCorruptTail reports that Open discarded an incomplete final JSONL
// record. The caller can surface this as a health warning without rejecting a
// journal whose complete prefix was safely recovered.
func (s *Store) RecoveredCorruptTail() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.recoveredTail
}

func (s *Store) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.file == nil {
		return nil
	}
	if err := s.file.Sync(); err != nil {
		_ = s.file.Close()
		s.file = nil
		return err
	}
	err := s.file.Close()
	s.file = nil
	return err
}
