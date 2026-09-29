package timesync

import (
	"context"
	"encoding/binary"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestNTPResponseValidation(t *testing.T) {
	valid := make([]byte, packetSize)
	valid[0] = 0x24 // NTP v4 server, leap indicator zero
	valid[1] = 3    // valid stratum; regression for mistaken 0x3 rejection
	want := time.Date(2026, 9, 29, 4, 0, 0, 0, time.UTC)
	binary.BigEndian.PutUint32(valid[40:44], uint32(want.Unix()+ntpEpochOffset))
	got, err := parseNTPResponse(valid)
	if err != nil || !got.Equal(want) {
		t.Fatalf("valid stratum-3 reply: time=%v err=%v", got, err)
	}
	for _, mutate := range []func([]byte){
		func(b []byte) { b[1] = 0 },
		func(b []byte) { b[1] = 16 },
		func(b []byte) { b[0] = 0xe4 }, // unsynchronised leap indicator
		func(b []byte) { b[0] = 0x23 }, // client-mode reply
		func(b []byte) { binary.BigEndian.PutUint32(b[40:44], ntpEpochOffset) },
	} {
		bad := append([]byte(nil), valid...)
		mutate(bad)
		if _, err := parseNTPResponse(bad); err == nil {
			t.Fatalf("invalid NTP packet accepted: %v", bad[:2])
		}
	}
}

func TestHTTPDateFallback(t *testing.T) {
	want := time.Date(2026, 9, 29, 4, 0, 0, 0, time.UTC)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodHead {
			t.Errorf("request method = %s", r.Method)
		}
		w.Header().Set("Date", want.Format(http.TimeFormat))
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()
	var applied time.Time
	source, err := syncHTTP(context.Background(), []string{server.URL}, time.Second,
		func(t time.Time) error { applied = t; return nil })
	if err != nil || source != server.URL || !applied.Equal(want) {
		t.Fatalf("HTTP time fallback: source=%s applied=%v err=%v", source, applied, err)
	}
}
