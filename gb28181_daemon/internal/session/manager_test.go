package session

import (
	"sync"
	"testing"
)

func TestSingleSlotLifecycleAndRetryCleanup(t *testing.T) {
	m := NewManager()
	if !m.Reserve("call-1", Live{ChannelID: "channel-1"}) {
		t.Fatal("first INVITE should reserve the slot")
	}
	if m.Reserve("call-2", Live{}) {
		t.Fatal("concurrent INVITE should be busy")
	}
	if got := m.Activate("call-1"); got == nil || got.StartedAt.IsZero() {
		t.Fatal("ACK/media start should activate the reservation")
	}
	if got := m.Cancel("call-1"); got != nil {
		t.Fatal("late failure callback must not delete an active session")
	}
	if !m.MarkCleanup("call-1") || !m.NeedsCleanup("call-1") {
		t.Fatal("failed stop must leave a retryable cleanup entry")
	}
	if m.Reserve("call-2", Live{}) {
		t.Fatal("cleanup pending must still own the media slot")
	}
	if got := m.Stop("call-1"); got == nil || got.CallID != "call-1" {
		t.Fatal("confirmed stop should release the matching call")
	}
	if got := m.Stop("call-1"); got != nil {
		t.Fatal("duplicate stop should be idempotent")
	}
	if !m.Reserve("call-2", Live{}) {
		t.Fatal("slot should accept next INVITE after cleanup")
	}
	if got := m.Cancel("call-2"); got == nil {
		t.Fatal("ACK timeout should release pending reservation")
	}
}

func TestConcurrentReserveHasOneWinner(t *testing.T) {
	m := NewManager()
	var wg sync.WaitGroup
	var mu sync.Mutex
	winners := 0
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if m.Reserve(string(rune('a'+i)), Live{}) {
				mu.Lock()
				winners++
				mu.Unlock()
			}
		}(i)
	}
	wg.Wait()
	if winners != 1 || m.Len() != 1 {
		t.Fatalf("single slot: winners=%d entries=%d", winners, m.Len())
	}
}
