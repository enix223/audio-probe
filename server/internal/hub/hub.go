// Package hub tracks the currently connected device sessions.
package hub

import (
	"sync"

	"audio-probe/server/internal/session"
)

type Hub struct {
	mu       sync.RWMutex
	sessions map[string]*session.Session
}

func New() *Hub {
	return &Hub{sessions: make(map[string]*session.Session)}
}

func (h *Hub) Add(s *session.Session) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.sessions[s.DeviceID()] = s
}

// Remove deletes the entry only if it still points at the same capture, so a
// reconnecting device cannot have its new session removed by the old reader.
func (h *Hub) Remove(deviceID, captureID string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if s, ok := h.sessions[deviceID]; ok && s.CaptureID() == captureID {
		delete(h.sessions, deviceID)
	}
}

func (h *Hub) Get(deviceID string) *session.Session {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.sessions[deviceID]
}

func (h *Hub) ByCapture(captureID string) *session.Session {
	h.mu.RLock()
	defer h.mu.RUnlock()
	for _, s := range h.sessions {
		if s.CaptureID() == captureID {
			return s
		}
	}
	return nil
}

func (h *Hub) List() []*session.Session {
	h.mu.RLock()
	defer h.mu.RUnlock()
	out := make([]*session.Session, 0, len(h.sessions))
	for _, s := range h.sessions {
		out = append(out, s)
	}
	return out
}
