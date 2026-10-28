package protocol

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func goldenDir(t *testing.T) string {
	t.Helper()
	dir := filepath.Join("..", "..", "..", "protocol", "golden")
	if _, err := os.Stat(dir); err != nil {
		t.Fatalf("golden dir: %v", err)
	}
	return dir
}

type expectedHeader struct {
	Version      uint8  `json:"version"`
	Type         uint8  `json:"type"`
	Flags        uint8  `json:"flags"`
	Format       uint8  `json:"format"`
	SegmentID    uint32 `json:"segment_id"`
	Seq          uint32 `json:"seq"`
	TimestampMS  uint32 `json:"timestamp_ms"`
	Samples      uint16 `json:"samples"`
	PayloadLen   uint16 `json:"payload_len"`
	RMSDBFSx100  int16  `json:"rms_dbfs_x100"`
	Peak         uint16 `json:"peak"`
	DCOffset     int16  `json:"dc_offset"`
	VADScore     uint8  `json:"vad_score"`
	Reserved     uint8  `json:"reserved"`
	PayloadBytes int    `json:"payload_len_actual"`
}

func TestGoldenAudioHeaders(t *testing.T) {
	dir := goldenDir(t)
	raw, err := os.ReadFile(filepath.Join(dir, "audio_header_expected.json"))
	if err != nil {
		t.Fatal(err)
	}
	var expected map[string]expectedHeader
	if err := json.Unmarshal(raw, &expected); err != nil {
		t.Fatal(err)
	}
	if len(expected) != 4 {
		t.Fatalf("expected 4 vectors, got %d", len(expected))
	}

	for name, want := range expected {
		data, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		h, err := DecodeAudioHeader(data, 8192)
		if err != nil {
			t.Fatalf("%s: decode: %v", name, err)
		}
		if h.Version != want.Version || h.Type != want.Type || h.Flags != want.Flags ||
			uint8(h.Format) != want.Format || h.SegmentID != want.SegmentID ||
			h.Seq != want.Seq || h.TimestampMS != want.TimestampMS ||
			h.Samples != want.Samples || h.PayloadLen != want.PayloadLen ||
			h.RMSDBFSx100 != want.RMSDBFSx100 || h.Peak != want.Peak ||
			h.DCOffset != want.DCOffset || h.VADScore != want.VADScore ||
			h.Reserved != want.Reserved {
			t.Fatalf("%s: header mismatch\ngot  %+v\nwant %+v", name, h, want)
		}
		if len(data)-AudioHeaderSize != want.PayloadBytes {
			t.Fatalf("%s: payload bytes %d, want %d", name, len(data)-AudioHeaderSize, want.PayloadBytes)
		}
		if h.Format == FormatPCMS16LE {
			if err := ValidatePCM(h, 1); err != nil {
				t.Fatalf("%s: pcm validation: %v", name, err)
			}
		}
	}
}

func TestGoldenMalformed(t *testing.T) {
	dir := goldenDir(t)
	raw, err := os.ReadFile(filepath.Join(dir, "malformed_expected.json"))
	if err != nil {
		t.Fatal(err)
	}
	var expected map[string]string
	if err := json.Unmarshal(raw, &expected); err != nil {
		t.Fatal(err)
	}
	if len(expected) != 10 {
		t.Fatalf("expected 10 malformed vectors, got %d", len(expected))
	}
	for name, wantCode := range expected {
		data, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		_, err = DecodeAudioHeader(data, 2048)
		if err == nil {
			t.Fatalf("%s: expected error %s, decoded successfully", name, wantCode)
		}
		he, ok := err.(*HeaderError)
		if !ok {
			t.Fatalf("%s: wrong error type %T", name, err)
		}
		if he.Code != wantCode {
			t.Fatalf("%s: got code %s, want %s", name, he.Code, wantCode)
		}
	}
}

func TestGoldenJSONFixturesParse(t *testing.T) {
	dir := goldenDir(t)

	read := func(name string) []byte {
		t.Helper()
		b, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatal(err)
		}
		return b
	}

	var hello Hello
	if err := json.Unmarshal(read("hello_linux_pcm.json"), &hello); err != nil {
		t.Fatal(err)
	}
	if err := ValidateHello(&hello); err != nil {
		t.Fatalf("linux hello: %v", err)
	}
	if !hello.Capabilities.HasFormat("pcm_s16le") || !hello.Capabilities.HasFormat("opus") {
		t.Fatal("linux hello should advertise pcm and opus")
	}

	var helloOpus Hello
	if err := json.Unmarshal(read("hello_ln882h_opus.json"), &helloOpus); err != nil {
		t.Fatal(err)
	}
	if err := ValidateHello(&helloOpus); err != nil {
		t.Fatalf("ln882h hello: %v", err)
	}
	if helloOpus.Capabilities.HasFormat("pcm_s16le") {
		t.Fatal("ln882h hello must not advertise unproven PCM")
	}
	if helloOpus.Capabilities.DevicePreRollMaxMS != 0 {
		t.Fatal("ln882h device pre-roll must be 0")
	}

	var ack HelloAck
	if err := json.Unmarshal(read("hello_ack_linux_pcm.json"), &ack); err != nil {
		t.Fatal(err)
	}
	if err := ValidateHelloAck(&ack); err != nil {
		t.Fatalf("linux hello_ack: %v", err)
	}
	if ack.Audio.Format != "pcm_s16le" || ack.Audio.SampleRate != 16000 {
		t.Fatalf("unexpected linux ack audio: %+v", ack.Audio)
	}

	var ackOpus HelloAck
	if err := json.Unmarshal(read("hello_ack_ln882h_opus.json"), &ackOpus); err != nil {
		t.Fatal(err)
	}
	if err := ValidateHelloAck(&ackOpus); err != nil {
		t.Fatalf("ln882h hello_ack: %v", err)
	}
	if ackOpus.Capture.Mode != "continuous" || ackOpus.Capture.VAD == nil ||
		ackOpus.Capture.VAD.Engine != "server" {
		t.Fatalf("ln882h ack must request continuous/server VAD: %+v", ackOpus.Capture)
	}

	var segStart SegmentStart
	if err := json.Unmarshal(read("segment_start_pcm.json"), &segStart); err != nil {
		t.Fatal(err)
	}
	if segStart.Type != "segment_start" || segStart.SegmentID != 12 || segStart.PreRollMS != 300 {
		t.Fatalf("segment_start fixture: %+v", segStart)
	}
	if segStart.OnsetTMS <= segStart.TMS {
		t.Fatal("onset must follow the pre-roll start")
	}

	var segEnd SegmentEnd
	if err := json.Unmarshal(read("segment_end_pcm.json"), &segEnd); err != nil {
		t.Fatal(err)
	}
	if segEnd.Type != "segment_end" || segEnd.Frames != 140 {
		t.Fatalf("segment_end fixture: %+v", segEnd)
	}

	var cmd MqttCommand
	if err := json.Unmarshal(read("mqtt_start.json"), &cmd); err != nil {
		t.Fatal(err)
	}
	parsed, err := ParseMqttCommand(read("mqtt_start.json"))
	if err != nil {
		t.Fatal(err)
	}
	if parsed.Action != "start" || parsed.ID.String() != "c1" {
		t.Fatalf("mqtt start: %+v", parsed)
	}

	if _, err := ParseMqttCommand(read("mqtt_stop.json")); err != nil {
		t.Fatalf("mqtt stop: %v", err)
	}

	var status MqttStatus
	if err := json.Unmarshal(read("mqtt_status.json"), &status); err != nil {
		t.Fatal(err)
	}
	if status.V != 1 || status.Type != "recorder_status" || status.State != "capturing" {
		t.Fatalf("mqtt status: %+v", status)
	}

	var tone TestTone
	if err := json.Unmarshal(read("test_tone_command.json"), &tone); err != nil {
		t.Fatal(err)
	}
	if tone.Type != "test_tone" || tone.FrequencyHz != 1000 || !tone.Capture {
		t.Fatalf("test tone: %+v", tone)
	}

	if _, err := ParseEnvelope(read("sweep_steps.json")); err != nil {
		t.Fatalf("sweep envelope: %v", err)
	}
}

func TestMQTTFlexID(t *testing.T) {
	for _, input := range []string{
		`{"v":1,"type":"recorder_cmd","id":"c1","action":"status"}`,
		`{"v":1,"type":"recorder_cmd","id":42,"action":"status"}`,
	} {
		cmd, err := ParseMqttCommand([]byte(input))
		if err != nil {
			t.Fatalf("%s: %v", input, err)
		}
		if cmd.ID.String() == "" {
			t.Fatalf("%s: empty id", input)
		}
	}
}

func TestHeaderRoundTrip(t *testing.T) {
	h := AudioHeader{
		Version: Version, Type: MessageTypeAudio, Flags: FlagPreRoll | FlagStatsValid,
		Format: FormatPCMS16LE, SegmentID: 9, Seq: 3, TimestampMS: 250,
		Samples: 320, PayloadLen: 640, RMSDBFSx100: -1234, Peak: 8000,
		DCOffset: -5, VADScore: 55,
	}
	raw := EncodeAudioHeader(h)
	if len(raw) != AudioHeaderSize {
		t.Fatalf("header size %d", len(raw))
	}
	msg := append(raw[:], make([]byte, 640)...)
	got, err := DecodeAudioHeader(msg, 8192)
	if err != nil {
		t.Fatal(err)
	}
	if got != h {
		t.Fatalf("round trip mismatch\ngot  %+v\nwant %+v", got, h)
	}
}
