package wsapi_test

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"

	"audio-probe/server/internal/httpapi"
	"audio-probe/server/internal/hub"
	"audio-probe/server/internal/metrics"
	"audio-probe/server/internal/protocol"
	"audio-probe/server/internal/storage"
	"audio-probe/server/internal/wsapi"
)

const pi = 3.14159265358979323846

const helloJSON = `{
  "type":"hello","protocol_version":1,
  "device":{"id":"unit-device","model":"linux-pcm","firmware":"0.1.0","chip":"linux",
            "audio_frontend":"miniaudio","boot_id":"abcd1234"},
  "clock":{"monotonic_ms":1000,"wall_ms":1760000000000,"synced":true,"capture_t0_mono_us":0},
  "control":{"owner":"mqtt","allow_ws_capture_control":false,"ws_mode":"on_demand",
             "mqtt_lost_stop_timeout_ms":30000,"max_capture_ms":1800000},
  "capabilities":{
    "formats":{
      "pcm_s16le":{"sample_rates":[16000,48000],"channels":[1,2],"frame_ms":[10,20,40,60],
                   "payload_max":4096,"channel_map":["mic0"]},
      "opus":{"sample_rates":[16000,48000],"channels":[1,2],"frame_ms":[10,20,40,60],
              "bitrate":[6000,64000],"payload_max":255,"encoders":["host"]}
    },
    "vad":["server","energy"],"loopback":false,"device_pre_roll_max_ms":5000,
    "server_pre_roll":true,"max_segment_ms":30000,"tx_queue_ms":3000,
    "limits":{"max_frame_bytes":8192}
  }
}`

type env struct {
	ts     *httptest.Server
	api    *httptest.Server
	layout *storage.Layout
	hub    *hub.Hub
}

func newEnv(t *testing.T, token string) *env {
	t.Helper()
	layout, err := storage.New(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	h := hub.New()
	logger := log.New(io.Discard, "", 0)
	opts := wsapi.DefaultOptions()
	opts.Token = token
	wsSrv := wsapi.New(opts, layout, h, logger)
	apiSrv := httpapi.New(h, layout, logger)
	ts := httptest.NewServer(wsSrv)
	api := httptest.NewServer(apiSrv.Handler())
	e := &env{ts: ts, api: api, layout: layout, hub: h}
	t.Cleanup(func() {
		// Wait for hijacked WebSocket handlers to finish their session writer
		// before the TempDir cleanup runs.
		deadline := time.Now().Add(8 * time.Second)
		for time.Now().Before(deadline) {
			if len(h.List()) == 0 {
				break
			}
			time.Sleep(10 * time.Millisecond)
		}
		ts.Close()
		api.Close()
	})
	return e
}

func wsURL(ts *httptest.Server) string {
	return "ws" + strings.TrimPrefix(ts.URL, "http") + "/v1/recorder"
}

func dial(t *testing.T, ts *httptest.Server, header http.Header) *websocket.Conn {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	opts := &websocket.DialOptions{HTTPHeader: header}
	conn, _, err := websocket.Dial(ctx, wsURL(ts), opts)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	return conn
}

func sendHello(t *testing.T, conn *websocket.Conn) protocol.HelloAck {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := conn.Write(ctx, websocket.MessageText, []byte(helloJSON)); err != nil {
		t.Fatalf("write hello: %v", err)
	}
	_, data, err := conn.Read(ctx)
	if err != nil {
		t.Fatalf("read hello_ack: %v", err)
	}
	var ack protocol.HelloAck
	if err := json.Unmarshal(data, &ack); err != nil {
		t.Fatalf("hello_ack json: %v", err)
	}
	if err := protocol.ValidateHelloAck(&ack); err != nil {
		t.Fatalf("hello_ack: %v", err)
	}
	return ack
}

func waitForSessions(t *testing.T, e *env, want int) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if len(e.hub.List()) == want {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("sessions = %d, want %d", len(e.hub.List()), want)
}

func sineFrame(seq uint32, samples int, speech bool) []byte {
	amp := 0.5 * 32767
	payload := make([]byte, samples*2)
	for i := 0; i < samples; i++ {
		v := int16(0)
		if speech {
			f := float64(uint32(seq)*320+uint32(i)) / 16000
			v = int16(math.Round(amp * math.Sin(2*pi*220*f)))
		}
		payload[i*2] = byte(uint16(v))
		payload[i*2+1] = byte(uint16(v) >> 8)
	}
	flags := uint8(protocol.FlagStatsValid)
	if speech {
		flags |= protocol.FlagSpeech
	}
	h := protocol.AudioHeader{
		Version: protocol.Version, Type: protocol.MessageTypeAudio, Flags: flags,
		Format: protocol.FormatPCMS16LE, SegmentID: 1, Seq: seq,
		TimestampMS: seq * 20, Samples: uint16(samples), PayloadLen: uint16(len(payload)),
		RMSDBFSx100: -900, Peak: 16384, DCOffset: 0, VADScore: 50,
	}
	hdr := protocol.EncodeAudioHeader(h)
	return append(hdr[:], payload...)
}

func TestWebSocketCaptureProducesReport(t *testing.T) {
	e := newEnv(t, "")
	conn := dial(t, e.ts, nil)
	defer conn.CloseNow()
	ack := sendHello(t, conn)
	waitForSessions(t, e, 1)
	if ack.Control.Owner != "mqtt" || ack.Start {
		t.Fatalf("unexpected control: %+v start=%v", ack.Control, ack.Start)
	}
	if ack.Audio.Format != "pcm_s16le" || ack.Audio.SampleRate != 16000 || ack.Audio.FrameMS != 20 {
		t.Fatalf("unexpected audio: %+v", ack.Audio)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	start := fmt.Sprintf(`{"type":"segment_start","capture_id":%q,"segment_id":1,
      "reason":"continuous","t_ms":0,"onset_t_ms":0,"pre_roll_ms":0,
      "audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"frame_ms":20},
      "gain_db":null,"tone":null}`, ack.CaptureID)
	if err := conn.Write(ctx, websocket.MessageText, []byte(start)); err != nil {
		t.Fatal(err)
	}
	for seq := 0; seq < 100; seq++ {
		speech := seq >= 20 && seq < 80
		if err := conn.Write(ctx, websocket.MessageBinary, sineFrame(uint32(seq), 320, speech)); err != nil {
			t.Fatalf("frame %d: %v", seq, err)
		}
	}
	end := fmt.Sprintf(`{"type":"segment_end","capture_id":%q,"segment_id":1,
      "reason":"silence","t_ms":2000,"frames":100,"frames_dropped":0,
      "pre_roll_frames":0,"duration_ms":2000,"speech_ms":1200,
      "rms_dbfs":-15.0,"peak_dbfs":-6.0,"clip_pct":0,"noise_dbfs":-90,"tx_drops":0}`,
		ack.CaptureID)
	if err := conn.Write(ctx, websocket.MessageText, []byte(end)); err != nil {
		t.Fatal(err)
	}
	_ = conn.Close(websocket.StatusNormalClosure, "done")

	dir := filepath.Join(e.layout.Root, "captures", ack.CaptureID)
	waitFor(t, filepath.Join(dir, "manifest.json"), 8*time.Second)
	waitFor(t, filepath.Join(dir, "summary.csv"), 8*time.Second)
	waitFor(t, filepath.Join(dir, "report.html"), 8*time.Second)
	segDir := filepath.Join(dir, "segments", "1")
	waitFor(t, filepath.Join(segDir, "metrics.json"), 8*time.Second)

	if st, err := os.Stat(filepath.Join(segDir, "audio.wav")); err != nil {
		t.Fatalf("wav: %v", err)
	} else if st.Size() != 44+100*320*2 {
		t.Fatalf("wav size %d, want %d", st.Size(), 44+100*320*2)
	}
	if st, err := os.Stat(filepath.Join(segDir, "frames.csv")); err != nil {
		t.Fatalf("frames.csv: %v", err)
	} else if st.Size() == 0 {
		t.Fatal("empty frames.csv")
	}
	if _, err := os.Stat(filepath.Join(segDir, "plots", "waveform.png")); err != nil {
		t.Fatalf("waveform plot: %v", err)
	}
	if _, err := os.Stat(filepath.Join(segDir, "plots", "spectrum.png")); err != nil {
		t.Fatalf("spectrum plot: %v", err)
	}

	raw, err := os.ReadFile(filepath.Join(segDir, "metrics.json"))
	if err != nil {
		t.Fatal(err)
	}
	var m metrics.SegmentMetrics
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatal(err)
	}
	if m.FramesReceived != 100 || m.FramesExpected != 100 || m.FramesLost != 0 {
		t.Fatalf("frame accounting: %+v", m)
	}
	if math.Abs(m.DurationMS-2000) > 1 {
		t.Fatalf("duration %.1f", m.DurationMS)
	}
	if m.SpeechMS < 1100 || m.SpeechMS > 1300 {
		t.Fatalf("speech %.1f", m.SpeechMS)
	}
	if m.NoiseFloorDBFS > -60 {
		t.Fatalf("noise floor %.1f", m.NoiseFloorDBFS)
	}
	if m.SNRDB < 20 {
		t.Fatalf("snr %.1f", m.SNRDB)
	}

	var manifest map[string]any
	rawManifest, _ := os.ReadFile(filepath.Join(dir, "manifest.json"))
	if err := json.Unmarshal(rawManifest, &manifest); err != nil {
		t.Fatal(err)
	}
	if manifest["format"] != "pcm_s16le" || manifest["device_id"] != "unit-device" {
		t.Fatalf("manifest: %+v", manifest)
	}
}

func TestWebSocketRejectsInvalidFrame(t *testing.T) {
	e := newEnv(t, "")
	conn := dial(t, e.ts, nil)
	defer conn.CloseNow()
	_ = sendHello(t, conn)
	waitForSessions(t, e, 1)

	// Header claims 640 payload bytes but the message carries none.
	h := protocol.AudioHeader{
		Version: protocol.Version, Type: protocol.MessageTypeAudio,
		Format: protocol.FormatPCMS16LE, SegmentID: 1, Seq: 0,
		Samples: 320, PayloadLen: 640,
	}
	hdr := protocol.EncodeAudioHeader(h)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = conn.Write(ctx, websocket.MessageBinary, hdr[:])
	var closeErr error
	for i := 0; i < 4; i++ {
		if _, _, err := conn.Read(ctx); err != nil {
			closeErr = err
			break
		}
	}
	if closeErr == nil {
		t.Fatal("expected protocol close")
	}
	if status := websocket.CloseStatus(closeErr); status != websocket.StatusCode(protocol.CloseProtocolError) {
		t.Fatalf("close status %v (%v)", status, closeErr)
	}
}

func TestWebSocketRequiresToken(t *testing.T) {
	e := newEnv(t, "secret")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if conn, _, err := websocket.Dial(ctx, wsURL(e.ts), nil); err == nil {
		conn.CloseNow()
		t.Fatal("dial without token succeeded")
	}
	header := http.Header{"Authorization": []string{"Bearer secret"}}
	conn := dial(t, e.ts, header)
	defer conn.CloseNow()
	_ = sendHello(t, conn)
	waitForSessions(t, e, 1)
}

func TestHTTPCommandRejectsStartUnderMQTTOwnership(t *testing.T) {
	e := newEnv(t, "")
	conn := dial(t, e.ts, nil)
	defer conn.CloseNow()
	_ = sendHello(t, conn)
	waitForSessions(t, e, 1)

	resp, err := http.Get(e.api.URL + "/api/v1/devices")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var devices struct {
		Devices []map[string]any `json:"devices"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&devices); err != nil {
		t.Fatal(err)
	}
	if len(devices.Devices) != 1 {
		t.Fatalf("devices = %d", len(devices.Devices))
	}
	body := strings.NewReader(`{"type":"start_capture"}`)
	r2, err := http.Post(e.api.URL+"/api/v1/devices/unit-device/commands",
		"application/json", body)
	if err != nil {
		t.Fatal(err)
	}
	defer r2.Body.Close()
	if r2.StatusCode != http.StatusConflict {
		data, _ := io.ReadAll(r2.Body)
		t.Fatalf("status %d: %s", r2.StatusCode, data)
	}
	data, _ := io.ReadAll(r2.Body)
	if !strings.Contains(string(data), protocol.ErrControlOwnedByMQTT) {
		t.Fatalf("body: %s", data)
	}
}

func TestEmbeddedFrontendIsServed(t *testing.T) {
	e := newEnv(t, "")
	resp, err := http.Get(e.api.URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("dashboard status %d", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	if !strings.Contains(string(body), `id="root"`) {
		t.Fatalf("dashboard is not the React build: %s", body)
	}
	// Client-side routes fall back to index.html.
	resp2, err := http.Get(e.api.URL + "/capture/some-id")
	if err != nil {
		t.Fatal(err)
	}
	defer resp2.Body.Close()
	body2, _ := io.ReadAll(resp2.Body)
	if resp2.StatusCode != http.StatusOK || !strings.Contains(string(body2), `id="root"`) {
		t.Fatalf("SPA fallback failed: status=%d body=%s", resp2.StatusCode, body2)
	}
}

func TestHealthAndCapturesAPI(t *testing.T) {
	e := newEnv(t, "")
	resp, err := http.Get(e.api.URL + "/api/v1/health")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("health %d", resp.StatusCode)
	}
	resp2, err := http.Get(e.api.URL + "/api/v1/captures")
	if err != nil {
		t.Fatal(err)
	}
	defer resp2.Body.Close()
	if resp2.StatusCode != http.StatusOK {
		t.Fatalf("captures %d", resp2.StatusCode)
	}
}

func waitFor(t *testing.T, path string, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(path); err == nil {
			return
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", path)
}
