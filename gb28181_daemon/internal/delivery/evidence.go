package delivery

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"xiaoyu/gb28181-daemon/internal/media"
	"xiaoyu/gb28181-daemon/internal/outbox"
)

// EvidenceUploader is independent of MessageSender so a slow or unavailable
// HTTP endpoint cannot delay standard GB28181 Alarm delivery.
type EvidenceUploader interface {
	Upload(context.Context, outbox.Record, []byte, []byte) error
}

// HTTPEvidenceUploader sends one multipart request per evidence_id. The
// endpoint is intentionally generic: a WVP-side adapter owns the platform
// specific association and can use the event/evidence metadata fields.
type HTTPEvidenceUploader struct {
	endpoint string
	token    string
	client   *http.Client
}

var ErrEvidenceCapacity = errors.New("evidence outbox capacity exhausted")

func NewHTTPEvidenceUploader(endpoint, token string, client *http.Client) (*HTTPEvidenceUploader, error) {
	endpoint = strings.TrimSpace(endpoint)
	if endpoint == "" {
		return nil, errors.New("evidence URL is required when evidence is enabled")
	}
	if strings.TrimSpace(token) == "" {
		return nil, errors.New("evidence bearer token is required when evidence is enabled")
	}
	if client == nil {
		client = &http.Client{}
	}
	return &HTTPEvidenceUploader{endpoint: endpoint, token: token, client: client}, nil
}

func (u *HTTPEvidenceUploader) Upload(ctx context.Context, record outbox.Record,
	metadata, image []byte) error {
	if strings.TrimSpace(record.Key) == "" {
		return errors.New("evidence outbox key is required")
	}
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	if err := writer.WriteField("event_id", record.Event.EventID); err != nil {
		return fmt.Errorf("write evidence event_id: %w", err)
	}
	if err := writer.WriteField("evidence_id", record.Event.EvidenceID); err != nil {
		return fmt.Errorf("write evidence evidence_id: %w", err)
	}
	metadataPart, err := writer.CreateFormField("metadata")
	if err != nil {
		return fmt.Errorf("create evidence metadata field: %w", err)
	}
	if _, err := metadataPart.Write(metadata); err != nil {
		return fmt.Errorf("write evidence metadata: %w", err)
	}
	imagePart, err := writer.CreateFormFile("image", record.Event.EvidenceID+".jpg")
	if err != nil {
		return fmt.Errorf("create evidence image field: %w", err)
	}
	if _, err := imagePart.Write(image); err != nil {
		return fmt.Errorf("write evidence image: %w", err)
	}
	if err := writer.Close(); err != nil {
		return fmt.Errorf("close evidence multipart body: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, u.endpoint, &body)
	if err != nil {
		return fmt.Errorf("create evidence request: %w", err)
	}
	req.Header.Set("Content-Type", writer.FormDataContentType())
	req.Header.Set("Idempotency-Key", record.Key)
	req.Header.Set("X-Evidence-ID", record.Event.EvidenceID)
	req.Header.Set("Authorization", "Bearer "+u.token)
	resp, err := u.client.Do(req)
	if err != nil {
		return fmt.Errorf("upload evidence %s: %w", record.Event.EvidenceID, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < http.StatusOK || resp.StatusCode >= http.StatusMultipleChoices {
		detail, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("upload evidence %s: HTTP %s: %s",
			record.Event.EvidenceID, resp.Status, strings.TrimSpace(string(detail)))
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	return nil
}

type evidenceMetadata struct {
	Version           uint32 `json:"v"`
	EvidenceID        string `json:"evidence_id"`
	EventID           string `json:"event_id"`
	ChannelID         string `json:"channel_id"`
	StreamEpoch       uint64 `json:"stream_epoch"`
	FrameID           uint64 `json:"frame_id"`
	SourcePTS         int64  `json:"source_pts"`
	Bytes             uint64 `json:"bytes"`
	SHA256            string `json:"sha256"`
	SourcePTSValid    bool   `json:"source_pts_valid"`
	SourceTimebaseNum uint32 `json:"source_timebase_num"`
	SourceTimebaseDen uint32 `json:"source_timebase_den"`
	CaptureTimeUS     int64  `json:"capture_time_us"`
	Width             uint32 `json:"width"`
	Height            uint32 `json:"height"`
	Accuracy          string `json:"accuracy"`
	FrameDelta        uint64 `json:"frame_delta"`
	PTSDelta          int64  `json:"pts_delta"`
	CreatedAtUS       int64  `json:"created_at_us"`
	ExpiresAtUS       int64  `json:"expires_at_us"`
	ObjectKey         string `json:"object_key"`
	StorageState      string `json:"storage_state"`
	DeliveryState     string `json:"delivery_state"`
}

func hasEvidenceMetadataFields(data []byte) (bool, error) {
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(data, &fields); err != nil {
		return false, err
	}
	for _, name := range []string{
		"v", "evidence_id", "event_id", "channel_id", "stream_epoch",
		"frame_id", "source_pts", "source_pts_valid", "source_timebase_num",
		"source_timebase_den", "capture_time_us", "width", "height", "bytes",
		"sha256", "accuracy", "frame_delta", "pts_delta", "created_at_us",
		"expires_at_us", "object_key", "storage_state", "delivery_state",
	} {
		if _, ok := fields[name]; !ok {
			return false, nil
		}
	}
	return true, nil
}

func loadEvidence(directory string, event media.AnalyticsEvent) ([]byte, []byte, error) {
	if strings.TrimSpace(directory) == "" {
		return nil, nil, errors.New("evidence directory is not configured")
	}
	if !validEvidenceComponent(event.EvidenceID) {
		return nil, nil, errors.New("event has an unsafe evidence_id")
	}
	metadataPath := filepath.Join(directory, event.EvidenceID+".json")
	imagePath := filepath.Join(directory, event.EvidenceID+".jpg")
	metadata, err := os.ReadFile(metadataPath)
	if err != nil {
		return nil, nil, fmt.Errorf("read evidence metadata: %w", err)
	}
	var info evidenceMetadata
	if err := json.Unmarshal(metadata, &info); err != nil {
		return nil, nil, fmt.Errorf("parse evidence metadata: %w", err)
	}
	complete, err := hasEvidenceMetadataFields(metadata)
	if err != nil {
		return nil, nil, fmt.Errorf("parse evidence metadata fields: %w", err)
	}
	if !complete {
		return nil, nil, errors.New("evidence metadata is incomplete")
	}
	if info.Version != 2 || info.EvidenceID != event.EvidenceID ||
		info.EventID != event.EventID || info.ChannelID != event.ChannelID ||
		info.StreamEpoch != event.StreamEpoch {
		return nil, nil, errors.New("evidence metadata identity does not match event")
	}
	if info.StreamEpoch == 0 || info.CaptureTimeUS <= 0 || info.Width == 0 ||
		info.Height == 0 || info.CreatedAtUS <= 0 || info.ExpiresAtUS <= 0 {
		return nil, nil, errors.New("evidence metadata has invalid dimensions or timestamps")
	}
	if info.SourcePTSValid && (info.SourceTimebaseNum == 0 || info.SourceTimebaseDen == 0) {
		return nil, nil, errors.New("evidence metadata source timebase is invalid")
	}
	if info.Accuracy != "exact" && info.Accuracy != "approximate" {
		return nil, nil, fmt.Errorf("evidence metadata accuracy is %q", info.Accuracy)
	}
	if info.ObjectKey != event.EvidenceID+".jpg" {
		return nil, nil, errors.New("evidence metadata object key does not match event")
	}
	if info.StorageState != "ready" {
		return nil, nil, fmt.Errorf("evidence storage state is %q", info.StorageState)
	}
	if info.DeliveryState != "awaiting_consumer" &&
		info.DeliveryState != "staged" {
		return nil, nil, fmt.Errorf("evidence delivery state is %q", info.DeliveryState)
	}
	image, err := os.ReadFile(imagePath)
	if err != nil {
		return nil, nil, fmt.Errorf("read evidence JPEG: %w", err)
	}
	if info.Bytes != uint64(len(image)) {
		return nil, nil, fmt.Errorf("evidence JPEG size is %d, metadata says %d",
			len(image), info.Bytes)
	}
	digest := sha256.Sum256(image)
	if !strings.EqualFold(info.SHA256, hex.EncodeToString(digest[:])) {
		return nil, nil, errors.New("evidence JPEG checksum mismatch")
	}
	return metadata, image, nil
}

func stageEvidence(sourceDir, destinationDir string, event media.AnalyticsEvent,
	maxBytes int64) error {
	if !validEvidenceComponent(event.EvidenceID) {
		return errors.New("event has an unsafe evidence_id")
	}
	if err := os.MkdirAll(destinationDir, 0700); err != nil {
		return fmt.Errorf("create evidence outbox directory: %w", err)
	}
	metadataPath := filepath.Join(destinationDir, event.EvidenceID+".json")
	imagePath := filepath.Join(destinationDir, event.EvidenceID+".jpg")
	if _, statErr := os.Stat(metadataPath); statErr == nil {
		if _, statErr = os.Stat(imagePath); statErr == nil {
			if _, _, verifyErr := loadEvidence(destinationDir, event); verifyErr == nil {
				return nil
			}
		}
	}
	metadata, image, err := loadEvidence(sourceDir, event)
	if err != nil {
		return err
	}
	if err := ensureEvidenceCapacity(destinationDir, event.EvidenceID,
		int64(len(metadata)+len(image)), maxBytes); err != nil {
		return err
	}
	if err := writeStagedFile(metadataPath, metadata); err != nil {
		return err
	}
	if err := writeStagedFile(imagePath, image); err != nil {
		_ = os.Remove(metadataPath)
		return err
	}
	if _, _, err := loadEvidence(destinationDir, event); err != nil {
		_ = os.Remove(metadataPath)
		_ = os.Remove(imagePath)
		return fmt.Errorf("verify staged evidence: %w", err)
	}
	return nil
}

func ensureEvidenceCapacity(directory, evidenceID string, incoming, maxBytes int64) error {
	if maxBytes <= 0 {
		return nil
	}
	used, err := evidenceDirectoryBytes(directory)
	if err != nil {
		return fmt.Errorf("measure evidence outbox: %w", err)
	}
	for _, suffix := range []string{".json", ".jpg"} {
		info, statErr := os.Stat(filepath.Join(directory, evidenceID+suffix))
		if statErr == nil {
			used -= info.Size()
		} else if !errors.Is(statErr, os.ErrNotExist) {
			return fmt.Errorf("stat existing evidence %s: %w", evidenceID+suffix, statErr)
		}
	}
	if incoming > maxBytes-used {
		return fmt.Errorf("%w: need %d bytes with %d bytes already used and %d byte limit",
			ErrEvidenceCapacity, incoming, used, maxBytes)
	}
	return nil
}

func evidenceDirectoryBytes(directory string) (int64, error) {
	entries, err := os.ReadDir(directory)
	if err != nil {
		return 0, err
	}
	var total int64
	for _, entry := range entries {
		info, err := entry.Info()
		if err != nil {
			return 0, err
		}
		if !info.Mode().IsRegular() {
			continue
		}
		if info.Size() > 0 && total > int64(^uint64(0)>>1)-info.Size() {
			return 0, errors.New("evidence outbox size overflow")
		}
		total += info.Size()
	}
	return total, nil
}

func removeStagedEvidence(directory string, event media.AnalyticsEvent) error {
	if strings.TrimSpace(directory) == "" || !validEvidenceComponent(event.EvidenceID) {
		return nil
	}
	var firstErr error
	for _, suffix := range []string{".json", ".jpg"} {
		if err := os.Remove(filepath.Join(directory, event.EvidenceID+suffix)); err != nil &&
			!errors.Is(err, os.ErrNotExist) && firstErr == nil {
			firstErr = err
		}
	}
	if firstErr != nil {
		return firstErr
	}
	return syncEvidenceDirectory(directory)
}

func cleanupOrphanedEvidence(directory string, records []outbox.Record) error {
	if strings.TrimSpace(directory) == "" {
		return nil
	}
	keep := make(map[string]struct{})
	for _, record := range records {
		if (record.EvidenceState == outbox.StatePending ||
			record.EvidenceState == outbox.StateStaging) &&
			validEvidenceComponent(record.Event.EvidenceID) {
			keep[record.Event.EvidenceID] = struct{}{}
		}
	}
	entries, err := os.ReadDir(directory)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".evidence-") {
			if err := os.Remove(filepath.Join(directory, name)); err != nil &&
				!errors.Is(err, os.ErrNotExist) {
				return err
			}
			continue
		}
		var evidenceID string
		switch {
		case strings.HasSuffix(name, ".json"):
			evidenceID = strings.TrimSuffix(name, ".json")
		case strings.HasSuffix(name, ".jpg"):
			evidenceID = strings.TrimSuffix(name, ".jpg")
		default:
			continue
		}
		if _, ok := keep[evidenceID]; ok {
			continue
		}
		if !validEvidenceComponent(evidenceID) {
			return fmt.Errorf("unsafe evidence outbox entry %q", name)
		}
		if err := removeStagedEvidence(directory, media.AnalyticsEvent{EvidenceID: evidenceID}); err != nil {
			return err
		}
	}
	return nil
}

func writeStagedFile(path string, data []byte) error {
	dir := filepath.Dir(path)
	temp, err := os.CreateTemp(dir, ".evidence-*")
	if err != nil {
		return fmt.Errorf("create staged evidence file: %w", err)
	}
	tempName := temp.Name()
	defer os.Remove(tempName)
	if err := temp.Chmod(0600); err != nil {
		_ = temp.Close()
		return fmt.Errorf("chmod staged evidence file: %w", err)
	}
	if _, err := temp.Write(data); err != nil {
		_ = temp.Close()
		return fmt.Errorf("write staged evidence file: %w", err)
	}
	if err := temp.Sync(); err != nil {
		_ = temp.Close()
		return fmt.Errorf("sync staged evidence file: %w", err)
	}
	if err := temp.Close(); err != nil {
		return fmt.Errorf("close staged evidence file: %w", err)
	}
	if err := os.Rename(tempName, path); err != nil {
		return fmt.Errorf("publish staged evidence file: %w", err)
	}
	if err := syncEvidenceDirectory(dir); err != nil {
		return fmt.Errorf("sync staged evidence directory: %w", err)
	}
	return nil
}

func syncEvidenceDirectory(path string) error {
	directory, err := os.Open(path)
	if err != nil {
		return err
	}
	defer directory.Close()
	return directory.Sync()
}

func validEvidenceComponent(value string) bool {
	if value == "" || filepath.Base(value) != value || value == "." || value == ".." {
		return false
	}
	for _, r := range value {
		if !(r >= 'a' && r <= 'z') && !(r >= 'A' && r <= 'Z') &&
			!(r >= '0' && r <= '9') && r != '-' && r != '_' && r != '.' {
			return false
		}
	}
	return true
}
