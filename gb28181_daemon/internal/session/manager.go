// Package session tracks pending and active GB28181 live sessions. The SIP
// layer works with Call-ID strings; media layer works with session IDs (same
// value for now), and this manager is the single place that maps between
// them. A pending entry is reserved before the INVITE 200 response is sent so
// a BYE racing with ACK cannot start an already cancelled media session.
package session

import (
	"sync"
	"time"
)

// Live describes one active live session.
type Live struct {
	CallID    string
	ChannelID string
	SSRC      string
	DestIP    string
	DestPort  int
	StartedAt time.Time
}

type state uint8

const (
	statePending state = iota + 1
	stateActive
	stateCleanup
)

type entry struct {
	live  Live
	state state
}

// Manager is a small thread-safe registry of live sessions.
type Manager struct {
	mu   sync.Mutex
	live map[string]*entry
}

func NewManager() *Manager {
	return &Manager{live: make(map[string]*entry)}
}

// Reserve claims a Call-ID before a 200 OK is sent. It returns false when
// another pending or active session already owns the single media slot.
func (m *Manager) Reserve(callID string, l Live) bool {
	if callID == "" {
		return false
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if len(m.live) != 0 {
		return false
	}
	l.CallID = callID
	m.live[callID] = &entry{live: l, state: statePending}
	return true
}

// Activate marks a reservation as media-active after media.start_live has
// succeeded. A cancelled or unknown reservation returns nil.
func (m *Manager) Activate(callID string) *Live {
	m.mu.Lock()
	defer m.mu.Unlock()
	e := m.live[callID]
	if e == nil || e.state != statePending {
		return nil
	}
	e.state = stateActive
	e.live.StartedAt = time.Now()
	l := e.live
	return &l
}

// Cancel releases a reservation only when media was never started.
func (m *Manager) Cancel(callID string) *Live {
	m.mu.Lock()
	defer m.mu.Unlock()
	e := m.live[callID]
	if e == nil || e.state != statePending {
		return nil
	}
	delete(m.live, callID)
	l := e.live
	return &l
}

// MarkCleanup retains an uncertain media start or failed stop for retry.
// The media engine's actual state is checked before the slot is released.
func (m *Manager) MarkCleanup(callID string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	e := m.live[callID]
	if e == nil {
		return false
	}
	e.state = stateCleanup
	return true
}

func (m *Manager) NeedsCleanup(callID string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	e := m.live[callID]
	return e != nil && e.state == stateCleanup
}

// Stop removes a session after the media controller has acknowledged the
// stop. Callers intentionally keep the entry when the controller fails so a
// later retry can release the actual media branch.
func (m *Manager) Stop(callID string) *Live {
	m.mu.Lock()
	defer m.mu.Unlock()
	e := m.live[callID]
	if e == nil {
		return nil
	}
	delete(m.live, callID)
	l := e.live
	return &l
}

func (m *Manager) Get(callID string) *Live {
	m.mu.Lock()
	defer m.mu.Unlock()
	e := m.live[callID]
	if e == nil {
		return nil
	}
	l := e.live
	return &l
}

func (m *Manager) IsPending(callID string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	e := m.live[callID]
	return e != nil && e.state == statePending
}

// All returns a snapshot of live sessions (used on shutdown).
func (m *Manager) All() []Live {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]Live, 0, len(m.live))
	for _, e := range m.live {
		out = append(out, e.live)
	}
	return out
}

func (m *Manager) Len() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.live)
}
