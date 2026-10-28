// Package session owns one WebSocket session: capture/segment state, the
// on-disk writer, command correlation, and the capture manifest/report.
package session

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"audio-probe/server/internal/protocol"
	"audio-probe/server/internal/report"
	"audio-probe/server/internal/storage"
)

var (
	ErrClosed             = errors.New("session closed")
	ErrBackpressure       = errors.New("session writer queue full")
	ErrControlOwnedByMQTT = errors.New("control_owned_by_mqtt")
	ErrCommandTimeout     = errors.New("command timeout")
	ErrUnsupportedCommand = errors.New("unsupported command")
)

// Sender delivers one WebSocket text message. The wsapi writer serializes it.
type Sender interface {
	SendText(text string) error
}

type Config struct {
	SessionID     string
	CaptureID     string
	DeviceID      string
	Hello         protocol.Hello
	Control       protocol.ControlInfo
	Audio         protocol.AudioConfig
	Capture       protocol.CaptureConfig
	Telemetry     protocol.TelemetryConfig
	Limits        protocol.Limits
	Path          string
	GainDB        *float64
	MaxFrameBytes int
}

type Counters struct {
	FramesReceived int64 `json:"frames_received"`
	BytesReceived  int64 `json:"bytes_received"`
	Segments       int64 `json:"segments"`
	FramesLost     int64 `json:"frames_lost"`
	DeviceDrops    int64 `json:"device_drops"`
	DecodeErrors   int64 `json:"decode_errors"`
	Backpressure   int64 `json:"backpressure"`
}

type jobKind uint8

const (
	jobStart jobKind = iota
	jobFrame
	jobEnd
	jobTelemetry
	jobStats
)

type frameJob struct {
	header  protocol.AudioHeader
	payload []byte
	arrival time.Time
}

type job struct {
	kind      jobKind
	start     *protocol.SegmentStart
	end       *protocol.SegmentEnd
	frame     *frameJob
	telemetry *protocol.Telemetry
	raw       json.RawMessage
}

type Session struct {
	cfg     Config
	layout  *storage.Layout
	sender  Sender
	jobs    chan job
	done    chan struct{}
	started time.Time

	sendMu    sync.RWMutex
	closed    bool
	closeOnce sync.Once

	mu            sync.Mutex
	state         string
	segments      []report.SegmentRow
	lastTelemetry *protocol.Telemetry
	lastStats     json.RawMessage
	notes         []string
	tone          *protocol.ToneInfo
	counters      Counters
	endedAt       time.Time
	endReason     string

	pendingMu sync.Mutex
	pending   map[int64]chan json.RawMessage
	nextID    atomic.Int64

	// Writer-goroutine only.
	seg *segmentWriter
}

func New(cfg Config, layout *storage.Layout, sender Sender) (*Session, error) {
	if cfg.CaptureID == "" || cfg.SessionID == "" {
		return nil, fmt.Errorf("missing capture/session id")
	}
	if cfg.MaxFrameBytes <= 0 {
		cfg.MaxFrameBytes = 8192
	}
	if _, err := layout.EnsureCapture(cfg.CaptureID); err != nil {
		return nil, err
	}
	s := &Session{
		cfg:     cfg,
		layout:  layout,
		sender:  sender,
		jobs:    make(chan job, 256),
		done:    make(chan struct{}),
		started: time.Now().UTC(),
		state:   "connected",
		pending: make(map[int64]chan json.RawMessage),
	}
	go s.run()
	s.writeManifest()
	return s, nil
}

func (s *Session) Control() protocol.ControlInfo { return s.cfg.Control }
func (s *Session) CaptureID() string             { return s.cfg.CaptureID }
func (s *Session) SessionID() string             { return s.cfg.SessionID }
func (s *Session) DeviceID() string              { return s.cfg.DeviceID }
func (s *Session) Audio() protocol.AudioConfig   { return s.cfg.Audio }
func (s *Session) MaxFrameBytes() int            { return s.cfg.MaxFrameBytes }

func (s *Session) enqueue(j job) error {
	s.sendMu.RLock()
	defer s.sendMu.RUnlock()
	if s.closed {
		return ErrClosed
	}
	select {
	case s.jobs <- j:
		return nil
	default:
		s.mu.Lock()
		s.counters.Backpressure++
		s.mu.Unlock()
		return ErrBackpressure
	}
}

func (s *Session) EnqueueStart(ev protocol.SegmentStart) error {
	cp := ev
	return s.enqueue(job{kind: jobStart, start: &cp})
}

func (s *Session) EnqueueEnd(ev protocol.SegmentEnd) error {
	cp := ev
	return s.enqueue(job{kind: jobEnd, end: &cp})
}

func (s *Session) EnqueueFrame(h protocol.AudioHeader, payload []byte, arrival time.Time) error {
	cp := append([]byte(nil), payload...)
	return s.enqueue(job{kind: jobFrame, frame: &frameJob{header: h, payload: cp, arrival: arrival}})
}

func (s *Session) EnqueueTelemetry(t protocol.Telemetry) error {
	cp := t
	return s.enqueue(job{kind: jobTelemetry, telemetry: &cp})
}

func (s *Session) EnqueueStats(raw json.RawMessage) error {
	cp := append(json.RawMessage(nil), raw...)
	return s.enqueue(job{kind: jobStats, raw: cp})
}

func (s *Session) NoteCaptureState(cs protocol.CaptureState) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if cs.State != "" {
		s.state = cs.State
	}
	if cs.Tx != nil {
		s.counters.DeviceDrops = int64(cs.Tx.Drops)
	}
}

func (s *Session) SetTone(t *protocol.ToneInfo) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.tone = t
}

func (s *Session) LastTelemetry() *protocol.Telemetry {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.lastTelemetry
}

func (s *Session) run() {
	defer close(s.done)
	for j := range s.jobs {
		switch j.kind {
		case jobStart:
			s.startSegment(*j.start)
		case jobFrame:
			if s.seg == nil {
				s.provisionalStart(j.frame)
			}
			s.addFrame(j.frame)
		case jobEnd:
			s.endSegment(j.end)
		case jobTelemetry:
			s.storeTelemetry(j.telemetry)
		case jobStats:
			s.storeStats(j.raw)
		}
	}
	if s.seg != nil {
		s.finalizeSegment("transport_lost", nil)
	}
	s.finalizeCapture()
}

func (s *Session) startSegment(start protocol.SegmentStart) {
	if s.seg != nil {
		s.addNote("segment_start received while another segment was open")
		s.finalizeSegment("stop_capture", nil)
	}
	seg, err := newSegmentWriter(s, start)
	if err != nil {
		s.addNote("failed to create segment writer: " + err.Error())
		return
	}
	s.seg = seg
	s.mu.Lock()
	s.state = "capturing"
	s.counters.Segments++
	s.mu.Unlock()
}

func (s *Session) provisionalStart(f *frameJob) {
	s.addNote("audio arrived before segment_start; server provisionally opened the segment")
	start := protocol.SegmentStart{
		Type:      "segment_start",
		CaptureID: s.cfg.CaptureID,
		SegmentID: f.header.SegmentID,
		Reason:    "continuous",
		TMS:       f.header.TimestampMS,
		OnsetTMS:  f.header.TimestampMS,
		Audio:     s.cfg.Audio,
	}
	if seg, err := newSegmentWriter(s, start); err == nil {
		s.seg = seg
	} else {
		s.addNote("failed to create provisional segment writer: " + err.Error())
	}
}

func (s *Session) addFrame(f *frameJob) {
	if s.seg == nil {
		return
	}
	if err := s.seg.addFrame(f); err != nil {
		s.addNote("frame write failed: " + err.Error())
		s.mu.Lock()
		s.counters.DecodeErrors++
		s.mu.Unlock()
	}
	s.mu.Lock()
	s.counters.FramesReceived++
	s.counters.BytesReceived += int64(len(f.payload))
	s.mu.Unlock()
}

func (s *Session) endSegment(end *protocol.SegmentEnd) {
	if s.seg == nil {
		s.addNote("segment_end received without an open segment")
		return
	}
	s.finalizeSegment(end.Reason, end)
}

func (s *Session) finalizeSegment(reason string, end *protocol.SegmentEnd) {
	if s.seg == nil {
		return
	}
	row, note := s.seg.finalize(reason, end)
	if note != "" {
		s.addNote(note)
	}
	s.seg = nil
	s.mu.Lock()
	s.segments = append(s.segments, row)
	s.counters.FramesLost += int64(row.Lost)
	s.state = "idle"
	s.tone = nil
	s.mu.Unlock()
	// Keep the capture list live while the WebSocket session remains open
	// (ws_mode=always); finalizeCapture rewrites it again at close.
	s.writeManifest()
}

func (s *Session) storeTelemetry(t *protocol.Telemetry) {
	s.mu.Lock()
	s.lastTelemetry = t
	s.mu.Unlock()
	if dir, err := s.layout.CaptureDir(s.cfg.CaptureID); err == nil {
		if f, err := os.OpenFile(dir+"/telemetry.jsonl", os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
			data, _ := json.Marshal(t)
			_, _ = f.Write(append(data, '\n'))
			_ = f.Close()
		}
	}
}

func (s *Session) storeStats(raw json.RawMessage) {
	s.mu.Lock()
	s.lastStats = raw
	s.mu.Unlock()
	if dir, err := s.layout.CaptureDir(s.cfg.CaptureID); err == nil {
		if f, err := os.OpenFile(dir+"/stats.jsonl", os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
			_, _ = f.Write(append(append([]byte(nil), raw...), '\n'))
			_ = f.Close()
		}
	}
}

func (s *Session) addNote(note string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, n := range s.notes {
		if n == note {
			return
		}
	}
	s.notes = append(s.notes, note)
}

// Close stops the writer, finalizes an open segment, and writes the capture
// report. It is safe to call more than once.
func (s *Session) Close(reason string) {
	s.closeOnce.Do(func() {
		s.sendMu.Lock()
		s.closed = true
		s.mu.Lock()
		s.endReason = reason
		s.endedAt = time.Now().UTC()
		s.mu.Unlock()
		close(s.jobs)
		s.sendMu.Unlock()
		select {
		case <-s.done:
		case <-time.After(30 * time.Second):
			s.addNote("session close timed out waiting for the writer")
		}
	})
}

func (s *Session) writeManifest() {
	dir, err := s.layout.CaptureDir(s.cfg.CaptureID)
	if err != nil {
		return
	}
	s.mu.Lock()
	manifest := map[string]any{
		"capture_id":      s.cfg.CaptureID,
		"session_id":      s.cfg.SessionID,
		"device_id":       s.cfg.DeviceID,
		"device":          s.cfg.Hello.Device,
		"capabilities":    s.cfg.Hello.Capabilities,
		"control":         s.cfg.Control,
		"audio":           s.cfg.Audio,
		"capture":         s.cfg.Capture,
		"telemetry":       s.cfg.Telemetry,
		"limits":          s.cfg.Limits,
		"format":          s.cfg.Audio.Format,
		"sample_rate":     s.cfg.Audio.SampleRate,
		"channels":        s.cfg.Audio.Channels,
		"frame_ms":        s.cfg.Audio.FrameMS,
		"path":            s.cfg.Path,
		"gain_db":         s.cfg.GainDB,
		"max_frame_bytes": s.cfg.MaxFrameBytes,
		"started_at":      s.started,
		"state":           s.state,
		"segments":        s.segments,
		"counters":        s.counters,
		"notes":           s.notes,
	}
	if !s.endedAt.IsZero() {
		manifest["ended_at"] = s.endedAt
		manifest["end_reason"] = s.endReason
	}
	s.mu.Unlock()
	_ = storage.WriteJSON(dir+"/manifest.json", manifest)
}

func (s *Session) finalizeCapture() {
	s.mu.Lock()
	s.state = "closed"
	if s.endedAt.IsZero() {
		s.endedAt = time.Now().UTC()
	}
	s.mu.Unlock()
	s.writeManifest()

	dir, err := s.layout.CaptureDir(s.cfg.CaptureID)
	if err != nil {
		return
	}
	s.mu.Lock()
	rows := append([]report.SegmentRow(nil), s.segments...)
	notes := append([]string(nil), s.notes...)
	meta := map[string]any{
		"capture_id":  s.cfg.CaptureID,
		"session_id":  s.cfg.SessionID,
		"device_id":   s.cfg.DeviceID,
		"device":      s.cfg.Hello.Device.Model,
		"format":      s.cfg.Audio.Format,
		"sample_rate": s.cfg.Audio.SampleRate,
		"channels":    s.cfg.Audio.Channels,
		"frame_ms":    s.cfg.Audio.FrameMS,
		"path":        s.cfg.Path,
		"gain_db":     s.cfg.GainDB,
		"started_at":  s.started.Format(time.RFC3339),
		"segments":    len(rows),
	}
	if !s.endedAt.IsZero() {
		meta["ended_at"] = s.endedAt.Format(time.RFC3339)
		meta["end_reason"] = s.endReason
	}
	s.mu.Unlock()

	_ = report.WriteSummaryCSV(dir, rows)
	_ = report.WriteHTML(dir, "Capture "+s.cfg.CaptureID, meta, rows, notes)
}

// SendCommand sends a server command and waits for its terminal response.
func (s *Session) SendCommand(cmd map[string]any, timeout time.Duration) (json.RawMessage, error) {
	typ, _ := cmd["type"].(string)
	if typ == "" {
		return nil, ErrUnsupportedCommand
	}
	if (typ == "start_capture" || typ == "stop_capture") &&
		s.cfg.Control.Owner == "mqtt" && !s.cfg.Control.AllowWSCaptureControl {
		return nil, ErrControlOwnedByMQTT
	}
	id := s.nextID.Add(1)
	cmd["id"] = id
	data, err := json.Marshal(cmd)
	if err != nil {
		return nil, err
	}
	ch := make(chan json.RawMessage, 1)
	s.pendingMu.Lock()
	s.pending[id] = ch
	s.pendingMu.Unlock()
	defer func() {
		s.pendingMu.Lock()
		delete(s.pending, id)
		s.pendingMu.Unlock()
	}()
	if err := s.sender.SendText(string(data)); err != nil {
		return nil, err
	}
	select {
	case resp := <-ch:
		return resp, nil
	case <-time.After(timeout):
		return nil, ErrCommandTimeout
	case <-s.done:
		return nil, ErrClosed
	}
}

// DeliverResponse routes a terminal command response from the device.
func (s *Session) DeliverResponse(raw json.RawMessage) {
	var env struct {
		Type string `json:"type"`
		ID   int64  `json:"id"`
	}
	if json.Unmarshal(raw, &env) != nil || env.ID == 0 {
		return
	}
	switch env.Type {
	case "ack", "nack", "completed", "failed", "pong":
	default:
		return
	}
	s.pendingMu.Lock()
	ch := s.pending[env.ID]
	s.pendingMu.Unlock()
	if ch == nil {
		return
	}
	select {
	case ch <- append(json.RawMessage(nil), raw...):
	default:
	}
}

// Snapshot is the JSON-friendly live view used by the HTTP API/dashboard.
func (s *Session) Snapshot() map[string]any {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := map[string]any{
		"session_id": s.cfg.SessionID,
		"capture_id": s.cfg.CaptureID,
		"device_id":  s.cfg.DeviceID,
		"state":      s.state,
		"control":    s.cfg.Control,
		"audio":      s.cfg.Audio,
		"capture":    s.cfg.Capture,
		"counters":   s.counters,
		"segments":   s.segments,
		"notes":      s.notes,
	}
	if s.lastTelemetry != nil {
		out["last_telemetry"] = s.lastTelemetry
	}
	if s.lastStats != nil {
		out["last_stats"] = s.lastStats
	}
	return out
}
