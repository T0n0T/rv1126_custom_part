// Package timesync performs a small SNTP exchange before SIP registration.
// The firmware image does not provide an ntpd/chronyd applet, so the daemon
// owns the one-shot boot synchronization and leaves the clock running after
// it has been corrected. Failures are reported to the caller and do not stop
// registration; a unit with no NTP reachability can still be diagnosed from
// the explicit log entry.
package timesync

import (
	"context"
	"encoding/binary"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"

	"golang.org/x/sys/unix"
)

const (
	ntpEpochOffset = 2208988800
	packetSize     = 48
)

var earliestTrustedTime = time.Date(2024, 1, 1, 0, 0, 0, 0, time.UTC)

// SyncHTTP uses the RFC 7231 Date header from a trusted local HTTP service as
// a startup fallback when UDP/123 is unavailable on the board network.
func SyncHTTP(ctx context.Context, servers []string, timeout time.Duration) (string, error) {
	return syncHTTP(ctx, servers, timeout, setTime)
}

func syncHTTP(ctx context.Context, servers []string, timeout time.Duration,
	apply func(time.Time) error) (string, error) {
	if len(servers) == 0 {
		return "", fmt.Errorf("no HTTP time servers configured")
	}
	if timeout <= 0 {
		timeout = 2 * time.Second
	}
	client := &http.Client{Timeout: timeout, CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	var lastErr error
	for _, raw := range servers {
		server := strings.TrimSpace(raw)
		if server == "" {
			continue
		}
		u, err := url.Parse(server)
		if err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
			lastErr = fmt.Errorf("%s: invalid HTTP URL", server)
			continue
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodHead, server, nil)
		if err != nil {
			lastErr = fmt.Errorf("%s: build request: %w", server, err)
			continue
		}
		res, err := client.Do(req)
		if err != nil {
			lastErr = fmt.Errorf("%s: request: %w", server, err)
			continue
		}
		date := res.Header.Get("Date")
		res.Body.Close()
		if res.StatusCode < 200 || res.StatusCode >= 400 {
			lastErr = fmt.Errorf("%s: HTTP status %d", server, res.StatusCode)
			continue
		}
		t, err := http.ParseTime(date)
		if err != nil {
			lastErr = fmt.Errorf("%s: invalid Date header: %w", server, err)
			continue
		}
		if t.Before(earliestTrustedTime) {
			lastErr = fmt.Errorf("%s: Date header is before %s", server, earliestTrustedTime.Format(time.RFC3339))
			continue
		}
		if err := apply(t); err != nil {
			lastErr = fmt.Errorf("%s: %w", server, err)
			continue
		}
		return server, nil
	}
	if lastErr == nil {
		lastErr = fmt.Errorf("no usable HTTP time server")
	}
	return "", lastErr
}

// Sync queries each configured server until one returns a valid transmit
// timestamp, then sets CLOCK_REALTIME. Servers may be host:port or bare host;
// bare hosts use UDP port 123.
func Sync(ctx context.Context, servers []string, timeout time.Duration) (string, error) {
	if len(servers) == 0 {
		return "", fmt.Errorf("no NTP servers configured")
	}
	if timeout <= 0 {
		timeout = 2 * time.Second
	}
	var lastErr error
	for _, raw := range servers {
		server := strings.TrimSpace(raw)
		if server == "" {
			continue
		}
		if _, _, err := net.SplitHostPort(server); err != nil {
			server = net.JoinHostPort(server, "123")
		}
		if err := syncServer(ctx, server, timeout); err != nil {
			lastErr = fmt.Errorf("%s: %w", server, err)
			continue
		}
		return server, nil
	}
	if lastErr == nil {
		lastErr = fmt.Errorf("no usable NTP server")
	}
	return "", lastErr
}

func syncServer(ctx context.Context, server string, timeout time.Duration) error {
	addr, err := net.ResolveUDPAddr("udp", server)
	if err != nil {
		return fmt.Errorf("resolve: %w", err)
	}
	conn, err := net.DialUDP("udp", nil, addr)
	if err != nil {
		return fmt.Errorf("dial: %w", err)
	}
	defer conn.Close()
	deadline := time.Now().Add(timeout)
	if ctxDeadline, ok := ctx.Deadline(); ok && ctxDeadline.Before(deadline) {
		deadline = ctxDeadline
	}
	if err := conn.SetDeadline(deadline); err != nil {
		return fmt.Errorf("set deadline: %w", err)
	}
	var request [packetSize]byte
	request[0] = 0x23 // LI=0, VN=4, mode=3 (client)
	if _, err := conn.Write(request[:]); err != nil {
		return fmt.Errorf("write: %w", err)
	}
	var response [packetSize]byte
	n, err := conn.Read(response[:])
	if err != nil {
		return fmt.Errorf("read: %w", err)
	}
	if n != packetSize {
		return fmt.Errorf("short NTP reply: %d bytes", n)
	}
	t, err := parseNTPResponse(response[:])
	if err != nil {
		return err
	}
	return setTime(t)
}

func parseNTPResponse(response []byte) (time.Time, error) {
	if len(response) != packetSize {
		return time.Time{}, fmt.Errorf("NTP reply must be %d bytes", packetSize)
	}
	if response[0]>>6 == 3 {
		return time.Time{}, fmt.Errorf("NTP server clock unsynchronized")
	}
	if response[0]&0x7 != 4 {
		return time.Time{}, fmt.Errorf("invalid NTP mode %d", response[0]&0x7)
	}
	// Stratum 0 is kiss-of-death; 16 and above are unsynchronised.
	if response[1] == 0 || response[1] >= 16 {
		return time.Time{}, fmt.Errorf("server reported invalid stratum %d", response[1])
	}
	seconds := binary.BigEndian.Uint32(response[40:44])
	fraction := binary.BigEndian.Uint32(response[44:48])
	if seconds < ntpEpochOffset {
		return time.Time{}, fmt.Errorf("invalid NTP timestamp")
	}
	nsec := (uint64(fraction) * uint64(time.Second)) >> 32
	unixTime := (int64(seconds) - ntpEpochOffset) * int64(time.Second)
	unixTime += int64(nsec)
	t := time.Unix(0, unixTime).UTC()
	if t.Before(earliestTrustedTime) {
		return time.Time{}, fmt.Errorf("NTP time is before %s", earliestTrustedTime.Format(time.RFC3339))
	}
	return t, nil
}

func setTime(t time.Time) error {
	tv := unix.NsecToTimeval(t.UnixNano())
	if err := unix.Settimeofday(&tv); err != nil {
		return fmt.Errorf("set CLOCK_REALTIME: %w", err)
	}
	return nil
}
