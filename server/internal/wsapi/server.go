// Package wsapi implements the device-facing WebSocket data plane.
package wsapi

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"

	"audio-probe/server/internal/hub"
	"audio-probe/server/internal/metrics"
	"audio-probe/server/internal/protocol"
	"audio-probe/server/internal/session"
	"audio-probe/server/internal/storage"
)

type Options struct {
	Token         string
	MaxFrameBytes int
	MaxQueueMS    int
	Format        string // auto, pcm_s16le, opus
	SampleRate    int
	Channels      int
	FrameMS       int
	Mode          string // auto, vad, continuous
	PreRollMS     int
	MaxSegmentMS  int
	LevelInterval int
	StatsInterval int
	VAD           metrics.VADConfig
}

func DefaultOptions() Options {
	return Options{
		MaxFrameBytes: 8192,
		MaxQueueMS:    3000,
		Format:        "auto",
		SampleRate:    16000,
		Channels:      1,
		FrameMS:       20,
		Mode:          "auto",
		PreRollMS:     300,
		MaxSegmentMS:  15000,
		LevelInterval: 500,
		StatsInterval: 5000,
		VAD:           metrics.DefaultVADConfig(16000),
	}
}

type Server struct {
	opts   Options
	layout *storage.Layout
	hub    *hub.Hub
	log    *log.Logger
}

func New(opts Options, layout *storage.Layout, h *hub.Hub, logger *log.Logger) *Server {
	if opts.MaxFrameBytes <= 0 {
		opts.MaxFrameBytes = 8192
	}
	if opts.VAD.FrameMS == 0 {
		opts.VAD = metrics.DefaultVADConfig(opts.SampleRate)
	}
	if logger == nil {
		logger = log.Default()
	}
	return &Server{opts: opts, layout: layout, hub: h, log: logger}
}

type wsWriter struct {
	conn   *websocket.Conn
	ch     chan []byte
	done   chan struct{}
	mu     sync.Mutex
	closed bool
}

func newWriter(conn *websocket.Conn) *wsWriter {
	w := &wsWriter{conn: conn, ch: make(chan []byte, 64), done: make(chan struct{})}
	go w.run()
	return w
}

func (w *wsWriter) run() {
	defer close(w.done)
	ctx := context.Background()
	for data := range w.ch {
		wctx, cancel := context.WithTimeout(ctx, 15*time.Second)
		err := w.conn.Write(wctx, websocket.MessageText, data)
		cancel()
		if err != nil {
			return
		}
	}
}

func (w *wsWriter) SendText(text string) error {
	w.mu.Lock()
	if w.closed {
		w.mu.Unlock()
		return fmt.Errorf("writer closed")
	}
	w.mu.Unlock()
	select {
	case w.ch <- []byte(text):
		return nil
	default:
		return fmt.Errorf("writer queue full")
	}
}

func (w *wsWriter) Close() {
	w.mu.Lock()
	if w.closed {
		w.mu.Unlock()
		return
	}
	w.closed = true
	close(w.ch)
	w.mu.Unlock()
	select {
	case <-w.done:
	case <-time.After(5 * time.Second):
	}
}

type protocolError struct {
	code string
	msg  string
}

func (e *protocolError) Error() string { return e.code + ": " + e.msg }

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/v1/recorder" && r.URL.Path != "/ws" {
		http.NotFound(w, r)
		return
	}
	if s.opts.Token != "" {
		auth := r.Header.Get("Authorization")
		want := "Bearer " + s.opts.Token
		if subtle.ConstantTimeCompare([]byte(auth), []byte(want)) != 1 {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
	}

	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		InsecureSkipVerify: true,
		CompressionMode:    websocket.CompressionDisabled,
	})
	if err != nil {
		return
	}
	defer conn.CloseNow()
	conn.SetReadLimit(int64(s.opts.MaxFrameBytes*2 + 4096))
	writer := newWriter(conn)
	defer writer.Close()

	helloCtx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
	typ, data, err := conn.Read(helloCtx)
	cancel()
	if err != nil {
		_ = conn.Close(websocket.StatusCode(4408), "hello timeout")
		return
	}
	if typ != websocket.MessageText {
		_ = writer.SendText(jsonError(protocol.ErrBadMessage, "hello must be text"))
		_ = conn.Close(websocket.StatusCode(4400), "bad hello")
		return
	}
	var hello protocol.Hello
	if err := json.Unmarshal(data, &hello); err != nil {
		_ = writer.SendText(jsonError(protocol.ErrBadMessage, err.Error()))
		_ = conn.Close(websocket.StatusCode(4400), "bad hello")
		return
	}
	if err := protocol.ValidateHello(&hello); err != nil {
		code := protocol.ErrBadMessage
		if pe, ok := err.(interface{ Error() string }); ok {
			_ = pe
		}
		_ = writer.SendText(jsonError(code, err.Error()))
		s.log.Printf("hello rejected device=%q: %v", hello.Device.ID, err)
		_ = conn.Close(websocket.StatusCode(4400), "bad hello")
		return
	}

	audio, err := s.selectAudio(hello)
	if err != nil {
		_ = writer.SendText(jsonError(protocol.ErrUnsupportedConfig, err.Error()))
		_ = conn.Close(websocket.StatusCode(4400), "no supported config")
		return
	}
	capture := s.selectCapture(hello, audio)
	maxFrame := s.effectiveMaxFrame(hello, audio)
	limits := protocol.Limits{MaxFrameBytes: maxFrame, MaxQueueMS: s.opts.MaxQueueMS}
	telemetry := protocol.TelemetryConfig{FrameStats: true,
		LevelIntervalMS: s.opts.LevelInterval, StatsIntervalMS: s.opts.StatsInterval}
	control := hello.Control
	if control.WSMode == "" {
		control.WSMode = "on_demand"
	}

	captureID := storage.NewID("cap")
	sessionID := storage.NewID("sess")
	ack := protocol.HelloAck{
		Type:            "hello_ack",
		ProtocolVersion: protocol.Version,
		SessionID:       sessionID,
		CaptureID:       captureID,
		ServerTimeMS:    time.Now().UnixMilli(),
		Control:         control,
		Audio:           audio,
		Capture:         capture,
		Telemetry:       telemetry,
		Limits:          limits,
		Start:           control.Owner == "ws",
	}
	ackData, _ := json.Marshal(ack)
	if err := writer.SendText(string(ackData)); err != nil {
		_ = conn.Close(websocket.StatusCode(1011), "writer failed")
		return
	}

	sess, err := session.New(session.Config{
		SessionID:     sessionID,
		CaptureID:     captureID,
		DeviceID:      hello.Device.ID,
		Hello:         hello,
		Control:       control,
		Audio:         audio,
		Capture:       capture,
		Telemetry:     telemetry,
		Limits:        limits,
		Path:          "mic",
		MaxFrameBytes: maxFrame,
	}, s.layout, writer)
	if err != nil {
		s.log.Printf("session create failed: %v", err)
		_ = conn.Close(websocket.StatusCode(1011), "storage error")
		return
	}
	s.hub.Add(sess)
	defer s.hub.Remove(sess.DeviceID(), sess.CaptureID())
	closeReason := "transport_lost"
	defer func() {
		sess.Close(closeReason)
	}()

	s.log.Printf("session %s device=%s capture=%s format=%s rate=%d ch=%d mode=%s owner=%s",
		sessionID, hello.Device.ID, captureID, audio.Format, audio.SampleRate, audio.Channels,
		capture.Mode, control.Owner)

	for {
		typ, data, err := conn.Read(r.Context())
		if err != nil {
			break
		}
		if typ == websocket.MessageText {
			if err := s.handleText(sess, writer, data); err != nil {
				if pe, ok := err.(*protocolError); ok {
					_ = writer.SendText(jsonError(pe.code, pe.msg))
					_ = conn.Close(websocket.StatusCode(protocol.CloseProtocolError), pe.code)
				}
				closeReason = "protocol_error"
				break
			}
		} else if typ == websocket.MessageBinary {
			if err := s.handleBinary(sess, writer, data); err != nil {
				if pe, ok := err.(*protocolError); ok {
					_ = writer.SendText(jsonError(pe.code, pe.msg))
					_ = conn.Close(websocket.StatusCode(protocol.CloseProtocolError), pe.code)
				}
				closeReason = "protocol_error"
				break
			}
		}
	}
}

func (s *Server) handleText(sess *session.Session, writer *wsWriter, data []byte) error {
	msgType, err := protocol.ParseEnvelope(data)
	if err != nil {
		return &protocolError{code: protocol.ErrBadMessage, msg: err.Error()}
	}
	switch msgType {
	case "capture_state":
		var cs protocol.CaptureState
		if err := json.Unmarshal(data, &cs); err != nil {
			return &protocolError{code: protocol.ErrBadMessage, msg: err.Error()}
		}
		sess.NoteCaptureState(cs)
	case "segment_start":
		var ev protocol.SegmentStart
		if err := json.Unmarshal(data, &ev); err != nil {
			return &protocolError{code: protocol.ErrBadMessage, msg: err.Error()}
		}
		if ev.CaptureID != sess.CaptureID() {
			return &protocolError{code: protocol.ErrBadMessage, msg: "segment_start capture_id mismatch"}
		}
		if ev.Tone != nil {
			sess.SetTone(ev.Tone)
		}
		if err := sess.EnqueueStart(ev); err != nil {
			return &protocolError{code: protocol.ErrInternal, msg: err.Error()}
		}
	case "segment_end":
		var ev protocol.SegmentEnd
		if err := json.Unmarshal(data, &ev); err != nil {
			return &protocolError{code: protocol.ErrBadMessage, msg: err.Error()}
		}
		if ev.CaptureID != sess.CaptureID() {
			return &protocolError{code: protocol.ErrBadMessage, msg: "segment_end capture_id mismatch"}
		}
		if err := sess.EnqueueEnd(ev); err != nil {
			return &protocolError{code: protocol.ErrInternal, msg: err.Error()}
		}
	case "telemetry":
		var t protocol.Telemetry
		if err := json.Unmarshal(data, &t); err != nil {
			return &protocolError{code: protocol.ErrBadMessage, msg: err.Error()}
		}
		_ = sess.EnqueueTelemetry(t)
	case "stats":
		if err := sess.EnqueueStats(append(json.RawMessage(nil), data...)); err != nil {
			return &protocolError{code: protocol.ErrInternal, msg: err.Error()}
		}
	case "tone_start":
		// Tone metadata normally arrives in segment_start.tone; keep the id.
		var ts struct {
			ToneID string `json:"tone_id"`
		}
		if json.Unmarshal(data, &ts) == nil && ts.ToneID != "" {
			sess.SetTone(&protocol.ToneInfo{ToneID: ts.ToneID})
		}
	case "tone_end":
		// informational
	case "ack", "nack", "completed", "failed", "pong":
		sess.DeliverResponse(append(json.RawMessage(nil), data...))
	case "accepted":
		// long-command intermediate; terminal follows
	case "ping":
		var ping struct {
			ID   int64 `json:"id"`
			T0MS int64 `json:"t0_ms"`
		}
		_ = json.Unmarshal(data, &ping)
		t1 := time.Now().UnixMilli()
		pong := fmt.Sprintf(`{"type":"pong","id":%d,"t0_ms":%d,"t1_ms":%d,"server_time_ms":%d}`,
			ping.ID, ping.T0MS, t1, t1)
		_ = writer.SendText(pong)
	case "error":
		s.log.Printf("device error: %s", string(data))
	default:
		return &protocolError{code: protocol.ErrUnsupportedFeature, msg: "unknown message type: " + msgType}
	}
	return nil
}

func (s *Server) handleBinary(sess *session.Session, writer *wsWriter, data []byte) error {
	h, err := protocol.DecodeAudioHeader(data, sess.MaxFrameBytes())
	if err != nil {
		return &protocolError{code: protocol.ErrBadMessage, msg: err.Error()}
	}
	cfg := sess.Audio()
	if h.Format.String() != cfg.Format {
		return &protocolError{code: protocol.ErrBadMessage,
			msg: fmt.Sprintf("frame format %s does not match negotiated %s", h.Format, cfg.Format)}
	}
	if h.Format == protocol.FormatPCMS16LE {
		if err := protocol.ValidatePCM(h, cfg.Channels); err != nil {
			return &protocolError{code: protocol.ErrBadMessage, msg: err.Error()}
		}
		wantSamples := cfg.SampleRate * cfg.FrameMS / 1000
		if int(h.Samples) != wantSamples {
			return &protocolError{code: protocol.ErrBadMessage,
				msg: fmt.Sprintf("pcm frame has %d samples, negotiated %d ms at %d Hz = %d",
					h.Samples, cfg.FrameMS, cfg.SampleRate, wantSamples)}
		}
	}
	payload := data[protocol.AudioHeaderSize:]
	if err := sess.EnqueueFrame(h, payload, time.Now()); err != nil {
		if err == session.ErrBackpressure {
			return &protocolError{code: protocol.ErrInternal, msg: "server write queue full"}
		}
		return &protocolError{code: protocol.ErrInternal, msg: err.Error()}
	}
	return nil
}

func jsonError(code, msg string) string {
	data, _ := json.Marshal(map[string]any{
		"type": "error", "code": code, "message": msg, "fatal": false, "detail": map[string]any{},
	})
	return string(data)
}

func (s *Server) selectAudio(hello protocol.Hello) (protocol.AudioConfig, error) {
	caps := hello.Capabilities
	prefer := s.opts.Format
	if prefer == "" || prefer == "auto" {
		if caps.HasFormat("pcm_s16le") {
			prefer = "pcm_s16le"
		} else if caps.HasFormat("opus") {
			prefer = "opus"
		} else {
			return protocol.AudioConfig{}, fmt.Errorf("device advertises no supported format")
		}
	}
	fc, ok := caps.Formats[prefer]
	if !ok {
		return protocol.AudioConfig{}, fmt.Errorf("device does not advertise %s", prefer)
	}
	rate := pickInt(fc.SampleRates, s.opts.SampleRate)
	ch := pickInt(fc.Channels, s.opts.Channels)
	frame := pickInt(fc.FrameMS, s.opts.FrameMS)
	cfg := protocol.AudioConfig{Format: prefer, SampleRate: rate, Channels: ch, FrameMS: frame}
	if prefer == "pcm_s16le" {
		cfg.BitsPerSample = 16
		cfg.Endianness = "little"
		yes := true
		cfg.Interleaved = &yes
	} else {
		cfg.Opus = &protocol.OpusConfig{}
		if len(fc.Encoders) == 1 {
			cfg.Opus.Encoder = fc.Encoders[0]
		}
	}
	return cfg, nil
}

func (s *Server) selectCapture(hello protocol.Hello, audio protocol.AudioConfig) protocol.CaptureConfig {
	caps := hello.Capabilities
	mode := s.opts.Mode
	if mode == "" || mode == "auto" {
		if audio.Format == "pcm_s16le" {
			mode = "vad"
		} else {
			mode = "continuous"
		}
	}
	if mode == "vad" && !hasString(caps.VAD, "energy") && !hasString(caps.VAD, "device") {
		mode = "continuous"
	}
	preRoll := s.opts.PreRollMS
	if preRoll > caps.DevicePreRollMaxMS {
		preRoll = caps.DevicePreRollMaxMS
	}
	maxSegment := s.opts.MaxSegmentMS
	if caps.MaxSegmentMS > 0 && (maxSegment <= 0 || caps.MaxSegmentMS < maxSegment) {
		maxSegment = caps.MaxSegmentMS
	}
	engine := "energy"
	if mode == "continuous" || audio.Format == "opus" {
		engine = "server"
	}
	vad := protocol.VadConfig{
		Engine:            engine,
		OnsetMS:           s.opts.VAD.OnsetMS,
		SilenceMS:         s.opts.VAD.SilenceMS,
		MinSpeechMS:       s.opts.VAD.MinSpeechMS,
		ThresholdDB:       s.opts.VAD.ThresholdDB,
		SendShortSegments: false,
	}
	cfg := protocol.CaptureConfig{
		Mode:         mode,
		PreRollMS:    preRoll,
		MaxSegmentMS: maxSegment,
		VAD:          &vad,
	}
	if mode == "continuous" {
		cfg.SegmentMS = 10000
	}
	if mode == "vad" && engine == "server" {
		// A device that cannot run a local VAD is moved to continuous mode.
		cfg.Mode = "continuous"
		cfg.SegmentMS = 10000
	}
	return cfg
}

func (s *Server) effectiveMaxFrame(hello protocol.Hello, audio protocol.AudioConfig) int {
	max := s.opts.MaxFrameBytes
	if hello.Capabilities.Limits.MaxFrameBytes > 0 && hello.Capabilities.Limits.MaxFrameBytes < max {
		max = hello.Capabilities.Limits.MaxFrameBytes
	}
	if fc, ok := hello.Capabilities.Formats[audio.Format]; ok && fc.PayloadMax > 0 {
		if cap := fc.PayloadMax + protocol.AudioHeaderSize; cap < max {
			max = cap
		}
	}
	return max
}

func pickInt(list []int, preferred int) int {
	if len(list) == 0 {
		return preferred
	}
	for _, v := range list {
		if v == preferred {
			return v
		}
	}
	return list[0]
}

func hasString(list []string, want string) bool {
	for _, v := range list {
		if v == want {
			return true
		}
	}
	return false
}
