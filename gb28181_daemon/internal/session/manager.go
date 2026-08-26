// Package session tracks active GB28181 live sessions. The SIP layer works
// with Call-ID strings; media layer works with session IDs (same value for
// now), and this manager is the single place that maps between them.
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

// Manager is a small thread-safe registry of live sessions.
type Manager struct {
	mu   sync.Mutex
	live map[string]*Live
}

func NewManager() *Manager {
	return &Manager{live: make(map[string]*Live)}
}

func (m *Manager) Start(callID string, l Live) *Live {
	l.CallID = callID
	l.StartedAt = time.Now()
	m.mu.Lock()
	m.live[callID] = &l
	m.mu.Unlock()
	return &l
}

func (m *Manager) Stop(callID string) *Live {
	m.mu.Lock()
	defer m.mu.Unlock()
	l := m.live[callID]
	delete(m.live, callID)
	return l
}

func (m *Manager) Get(callID string) *Live {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.live[callID]
}

// All returns a snapshot of live sessions (used on shutdown).
func (m *Manager) All() []Live {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]Live, 0, len(m.live))
	for _, l := range m.live {
		out = append(out, *l)
	}
	return out
}

func (m *Manager) Len() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.live)
}
