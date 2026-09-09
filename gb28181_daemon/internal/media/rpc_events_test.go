package media

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"path/filepath"
	"testing"
	"time"
)

func TestConsumeEventsPersistsBeforeAckAndKeepsSocketOpen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "media.sock")
	listener, err := net.Listen("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()

	serverDone := make(chan error, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			serverDone <- err
			return
		}
		defer conn.Close()
		reader := bufio.NewReader(conn)
		var request rpcRequest
		line, err := reader.ReadBytes('\n')
		if err == nil {
			err = json.Unmarshal(line, &request)
		}
		if err != nil {
			serverDone <- err
			return
		}
		if request.Method != "media.subscribe_events" {
			serverDone <- &testError{"unexpected method: " + request.Method}
			return
		}
		var params eventSubscribeParams
		if err := json.Unmarshal(request.Params, &params); err != nil {
			serverDone <- err
			return
		}
		if params.AfterCursor != 4 {
			serverDone <- &testError{"unexpected after cursor"}
			return
		}
		encoder := json.NewEncoder(conn)
		if err := encoder.Encode(rpcEnvelope{
			V:      1,
			ID:     request.ID,
			Result: mustJSON(eventSubscribeResult{Subscribed: true, ReplayGap: true, LatestCursor: 5}),
		}); err != nil {
			serverDone <- err
			return
		}
		event := AnalyticsEvent{
			ContractVersion: 1,
			EventID:         "event-1",
			ChannelID:       "35020000001310000001",
			Phase:           "START",
			EventSeq:        1,
			FrameID:         90,
			SourcePTSValid:  true,
			SourcePTS:       9000,
			SourceTimebase:  Timebase{Num: 1, Den: 90000},
		}
		if err := encoder.Encode(rpcEnvelope{
			V:      1,
			Method: "media.event",
			Params: mustJSON(eventNotificationParams{
				Event:     "analytics",
				Cursor:    5,
				Analytics: event,
			}),
		}); err != nil {
			serverDone <- err
			return
		}
		var ack rpcRequest
		line, err = reader.ReadBytes('\n')
		if err == nil {
			err = json.Unmarshal(line, &ack)
		}
		if err != nil {
			serverDone <- err
			return
		}
		if ack.Method != "media.ack_events" {
			serverDone <- &testError{"expected media.ack_events"}
			return
		}
		var ackParams eventAckParams
		if err := json.Unmarshal(ack.Params, &ackParams); err != nil {
			serverDone <- err
			return
		}
		if ackParams.Cursor != 5 {
			serverDone <- &testError{"unexpected ACK cursor"}
			return
		}
		if err := encoder.Encode(rpcEnvelope{V: 1, ID: ack.ID, Result: mustJSON(map[string]uint64{"acked_cursor": 5})}); err != nil {
			serverDone <- err
			return
		}
		serverDone <- nil
	}()

	client := NewRPC(path, time.Second, nil)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	var info EventSubscribeInfo
	var got EventNotification
	err = client.ConsumeEvents(ctx, 4, func(value EventSubscribeInfo) error {
		info = value
		return nil
	}, func(value EventNotification) error {
		got = value
		return nil
	})
	if err == nil {
		t.Fatal("ConsumeEvents unexpectedly returned nil after server close")
	}
	if got.Cursor != 5 || got.Event.EventID != "event-1" {
		t.Fatalf("unexpected event: %+v", got)
	}
	if !info.ReplayGap || info.LatestCursor != 5 {
		t.Fatalf("unexpected subscription info: %+v", info)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
}

type testError struct{ message string }

func (e *testError) Error() string { return e.message }
