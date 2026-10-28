package protocol

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

// Error codes from docs/protocol.md §8.4.
const (
	ErrBadMessage         = "bad_message"
	ErrUnsupportedVersion = "unsupported_version"
	ErrUnsupportedFormat  = "unsupported_format"
	ErrUnsupportedConfig  = "unsupported_config"
	ErrUnsupportedFeature = "unsupported_feature"
	ErrInvalidState       = "invalid_state"
	ErrBusy               = "busy"
	ErrCaptureActive      = "capture_active"
	ErrCaptureNotActive   = "capture_not_active"
	ErrAudioHWError       = "audio_hw_error"
	ErrChipError          = "chip_error"
	ErrQueueOverflow      = "queue_overflow"
	ErrTimeout            = "timeout"
	ErrControlOwnedByMQTT = "control_owned_by_mqtt"
	ErrControlConflict    = "control_conflict"
	ErrWSUnavailable      = "ws_unavailable"
	ErrMQTTUnavailable    = "mqtt_unavailable"
	ErrExpired            = "expired"
	ErrNotProvisioned     = "not_provisioned"
	ErrInternal           = "internal"
	ErrUnsupportedAction  = "unsupported_action"
)

// Close codes.
const (
	CloseNormal          = 1000
	CloseProtocolError   = 1002
	ClosePolicyViolation = 1008
	CloseServerError     = 1011
	CloseBadHello        = 4400
	CloseAuthFailed      = 4401
	CloseHelloTimeout    = 4408
	CloseTooManySessions = 4429
)

// Envelope is the minimum shared shape of every JSON message.
type Envelope struct {
	Type string `json:"type"`
}

// ParseEnvelope returns the message type without validating the body.
func ParseEnvelope(data []byte) (string, error) {
	var e Envelope
	dec := json.NewDecoder(bytes.NewReader(data))
	if err := dec.Decode(&e); err != nil {
		return "", fmt.Errorf("%s: %w", ErrBadMessage, err)
	}
	if strings.TrimSpace(e.Type) == "" {
		return "", fmt.Errorf("%s: missing type", ErrBadMessage)
	}
	return e.Type, nil
}

// ---------------------------------------------------------------------------
// Capabilities and hello
// ---------------------------------------------------------------------------

type FormatCapability struct {
	SampleRates []int    `json:"sample_rates"`
	Channels    []int    `json:"channels"`
	FrameMS     []int    `json:"frame_ms"`
	Bitrate     []int    `json:"bitrate,omitempty"`
	PayloadMax  int      `json:"payload_max"`
	Encoders    []string `json:"encoders,omitempty"`
	ChannelMap  []string `json:"channel_map,omitempty"`
}

type ToneCapability struct {
	Waveforms      []string `json:"waveforms"`
	FrequencyHz    MinMax   `json:"frequency_hz"`
	MaxAmplitudeDB float64  `json:"max_amplitude_dbfs"`
}

type MinMax struct {
	Min float64 `json:"min"`
	Max float64 `json:"max"`
}

type SpeakerCapability struct {
	Min  int    `json:"min"`
	Max  int    `json:"max"`
	Unit string `json:"unit"`
}

type Limits struct {
	MaxFrameBytes int `json:"max_frame_bytes"`
	MaxQueueMS    int `json:"max_queue_ms,omitempty"`
}

type ControlInfo struct {
	Owner                 string `json:"owner"`
	AllowWSCaptureControl bool   `json:"allow_ws_capture_control"`
	WSMode                string `json:"ws_mode"`
	MQTTLostStopTimeoutMS int    `json:"mqtt_lost_stop_timeout_ms,omitempty"`
	MaxCaptureMS          int    `json:"max_capture_ms,omitempty"`
}

type DeviceInfo struct {
	ID            string `json:"id"`
	Model         string `json:"model"`
	Firmware      string `json:"firmware"`
	Chip          string `json:"chip"`
	AudioFrontend string `json:"audio_frontend"`
	BootID        string `json:"boot_id"`
}

type ClockInfo struct {
	MonotonicMS int64 `json:"monotonic_ms"`
	WallMS      int64 `json:"wall_ms"`
	Synced      bool  `json:"synced"`
	CaptureT0US int64 `json:"capture_t0_mono_us"`
}

type Capabilities struct {
	Formats            map[string]FormatCapability `json:"formats"`
	VAD                []string                    `json:"vad"`
	Gain               *MinMax                     `json:"gain,omitempty"`
	Tone               *ToneCapability             `json:"test_tone,omitempty"`
	Speaker            *SpeakerCapability          `json:"speaker,omitempty"`
	Loopback           bool                        `json:"loopback"`
	DevicePreRollMaxMS int                         `json:"device_pre_roll_max_ms"`
	ServerPreRoll      bool                        `json:"server_pre_roll"`
	MaxSegmentMS       int                         `json:"max_segment_ms"`
	TxQueueMS          int                         `json:"tx_queue_ms"`
	Limits             Limits                      `json:"limits"`
}

type Hello struct {
	Type            string       `json:"type"`
	ProtocolVersion int          `json:"protocol_version"`
	Device          DeviceInfo   `json:"device"`
	Clock           ClockInfo    `json:"clock"`
	Control         ControlInfo  `json:"control"`
	Capabilities    Capabilities `json:"capabilities"`
}

func ValidateHello(h *Hello) error {
	if h.Type != "hello" {
		return fmt.Errorf("%s: expected hello", ErrBadMessage)
	}
	if h.ProtocolVersion != Version {
		return fmt.Errorf("%s: protocol_version %d", ErrUnsupportedVersion, h.ProtocolVersion)
	}
	if strings.TrimSpace(h.Device.ID) == "" {
		return fmt.Errorf("%s: device.id", ErrBadMessage)
	}
	if len(h.Capabilities.Formats) == 0 {
		return fmt.Errorf("%s: no formats", ErrUnsupportedConfig)
	}
	for name, f := range h.Capabilities.Formats {
		if name != "pcm_s16le" && name != "opus" {
			return fmt.Errorf("%s: format %q", ErrUnsupportedFormat, name)
		}
		if len(f.SampleRates) == 0 || len(f.Channels) == 0 || len(f.FrameMS) == 0 {
			return fmt.Errorf("%s: incomplete %s capability", ErrUnsupportedConfig, name)
		}
		if f.PayloadMax <= 0 {
			return fmt.Errorf("%s: %s payload_max", ErrUnsupportedConfig, name)
		}
	}
	if h.Control.Owner != "mqtt" && h.Control.Owner != "ws" {
		return fmt.Errorf("%s: control.owner %q", ErrUnsupportedConfig, h.Control.Owner)
	}
	if h.Control.WSMode != "" && h.Control.WSMode != "on_demand" && h.Control.WSMode != "always" {
		return fmt.Errorf("%s: ws_mode %q", ErrUnsupportedConfig, h.Control.WSMode)
	}
	return nil
}

// HasFormat reports whether the device advertised a usable format.
func (c Capabilities) HasFormat(name string) bool {
	f, ok := c.Formats[name]
	return ok && len(f.SampleRates) > 0 && len(f.Channels) > 0 && len(f.FrameMS) > 0
}

func intIn(v int, list []int) bool {
	for _, x := range list {
		if x == v {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// hello_ack and audio/capture configuration
// ---------------------------------------------------------------------------

type OpusConfig struct {
	Encoder     string `json:"encoder,omitempty"`
	Bitrate     int    `json:"bitrate,omitempty"`
	Complexity  int    `json:"complexity,omitempty"`
	VBR         *bool  `json:"vbr,omitempty"`
	Application string `json:"application,omitempty"`
}

type AudioConfig struct {
	Format        string      `json:"format"`
	SampleRate    int         `json:"sample_rate"`
	Channels      int         `json:"channels"`
	FrameMS       int         `json:"frame_ms"`
	BitsPerSample int         `json:"bits_per_sample,omitempty"`
	Endianness    string      `json:"endianness,omitempty"`
	Interleaved   *bool       `json:"interleaved,omitempty"`
	Opus          *OpusConfig `json:"opus,omitempty"`
}

func (a AudioConfig) BytesPerFrame() int {
	if a.Format == "pcm_s16le" {
		return a.SampleRate / 1000 * a.FrameMS * a.Channels * 2
	}
	return 0
}

func (a AudioConfig) Validate() error {
	switch a.Format {
	case "pcm_s16le", "opus":
	default:
		return fmt.Errorf("%s: format %q", ErrUnsupportedFormat, a.Format)
	}
	if a.SampleRate <= 0 || a.Channels <= 0 || a.FrameMS <= 0 {
		return fmt.Errorf("%s: audio rate/channels/frame", ErrUnsupportedConfig)
	}
	if a.Format == "pcm_s16le" {
		if a.BitsPerSample != 0 && a.BitsPerSample != 16 {
			return fmt.Errorf("%s: pcm bits_per_sample", ErrUnsupportedFormat)
		}
		if a.Endianness != "" && a.Endianness != "little" {
			return fmt.Errorf("%s: pcm endianness", ErrUnsupportedFormat)
		}
		if a.Interleaved != nil && !*a.Interleaved {
			return fmt.Errorf("%s: pcm must be interleaved", ErrUnsupportedFormat)
		}
	}
	return nil
}

type VadConfig struct {
	Engine            string  `json:"engine,omitempty"`
	OnsetMS           int     `json:"onset_ms,omitempty"`
	SilenceMS         int     `json:"silence_ms,omitempty"`
	MinSpeechMS       int     `json:"min_speech_ms,omitempty"`
	GuardMS           int     `json:"guard_ms,omitempty"`
	ThresholdDB       float64 `json:"threshold_db,omitempty"`
	SendShortSegments bool    `json:"send_short_segments,omitempty"`
}

type CaptureConfig struct {
	Mode         string     `json:"mode,omitempty"`
	PreRollMS    int        `json:"pre_roll_ms,omitempty"`
	MaxSegmentMS int        `json:"max_segment_ms,omitempty"`
	SegmentMS    int        `json:"segment_ms,omitempty"`
	DurationMS   int        `json:"duration_ms,omitempty"`
	VAD          *VadConfig `json:"vad,omitempty"`
	GainDB       *float64   `json:"gain_db,omitempty"`
}

type TelemetryConfig struct {
	FrameStats      bool `json:"frame_stats"`
	LevelIntervalMS int  `json:"level_interval_ms,omitempty"`
	StatsIntervalMS int  `json:"stats_interval_ms,omitempty"`
}

type HelloAck struct {
	Type            string          `json:"type"`
	ProtocolVersion int             `json:"protocol_version"`
	SessionID       string          `json:"session_id"`
	CaptureID       string          `json:"capture_id"`
	ServerTimeMS    int64           `json:"server_time_ms"`
	Control         ControlInfo     `json:"control"`
	Audio           AudioConfig     `json:"audio"`
	Capture         CaptureConfig   `json:"capture"`
	Telemetry       TelemetryConfig `json:"telemetry"`
	Limits          Limits          `json:"limits"`
	Start           bool            `json:"start"`
}

func ValidateHelloAck(a *HelloAck) error {
	if a.Type != "hello_ack" {
		return fmt.Errorf("%s: expected hello_ack", ErrBadMessage)
	}
	if a.ProtocolVersion != Version {
		return fmt.Errorf("%s: protocol_version %d", ErrUnsupportedVersion, a.ProtocolVersion)
	}
	if a.SessionID == "" || a.CaptureID == "" {
		return fmt.Errorf("%s: session_id/capture_id", ErrBadMessage)
	}
	if err := a.Audio.Validate(); err != nil {
		return err
	}
	if a.Limits.MaxFrameBytes <= 0 {
		return fmt.Errorf("%s: limits.max_frame_bytes", ErrBadMessage)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Device -> server messages
// ---------------------------------------------------------------------------

type CaptureState struct {
	Type          string         `json:"type"`
	State         string         `json:"state"`
	CaptureID     string         `json:"capture_id,omitempty"`
	Mode          string         `json:"mode,omitempty"`
	Audio         *AudioConfig   `json:"audio,omitempty"`
	Capture       *CaptureConfig `json:"capture,omitempty"`
	GainDB        *float64       `json:"gain_db,omitempty"`
	GainSupported bool           `json:"gain_supported"`
	Tx            *TxState       `json:"tx,omitempty"`
}

type TxState struct {
	QueueMS    int `json:"queue_ms"`
	Drops      int `json:"drops"`
	Errors     int `json:"errors"`
	Reconnects int `json:"reconnects"`
}

type ToneInfo struct {
	ToneID        string  `json:"tone_id"`
	Waveform      string  `json:"waveform"`
	FrequencyHz   float64 `json:"frequency_hz,omitempty"`
	StartHz       float64 `json:"start_hz,omitempty"`
	EndHz         float64 `json:"end_hz,omitempty"`
	SweepMode     string  `json:"sweep_mode,omitempty"`
	AmplitudeDBFS float64 `json:"amplitude_dbfs"`
	SpeakerVolume int     `json:"speaker_volume"`
	CapturePath   string  `json:"capture_path,omitempty"`
	Loopback      bool    `json:"loopback"`
	FadeMS        int     `json:"fade_ms"`
}

type SegmentStart struct {
	Type      string      `json:"type"`
	CaptureID string      `json:"capture_id"`
	SegmentID uint32      `json:"segment_id"`
	Reason    string      `json:"reason"`
	TMS       uint32      `json:"t_ms"`
	OnsetTMS  uint32      `json:"onset_t_ms,omitempty"`
	PreRollMS int         `json:"pre_roll_ms"`
	Audio     AudioConfig `json:"audio"`
	GainDB    *float64    `json:"gain_db"`
	Label     string      `json:"label,omitempty"`
	Tone      *ToneInfo   `json:"tone"`
}

type SegmentEnd struct {
	Type          string  `json:"type"`
	CaptureID     string  `json:"capture_id"`
	SegmentID     uint32  `json:"segment_id"`
	Reason        string  `json:"reason"`
	TMS           uint32  `json:"t_ms"`
	Frames        int     `json:"frames"`
	FramesDropped int     `json:"frames_dropped"`
	PreRollFrames int     `json:"pre_roll_frames"`
	DurationMS    int     `json:"duration_ms"`
	SpeechMS      int     `json:"speech_ms"`
	RMSDBFS       float64 `json:"rms_dbfs"`
	PeakDBFS      float64 `json:"peak_dbfs"`
	ClipPct       float64 `json:"clip_pct"`
	NoiseDBFS     float64 `json:"noise_dbfs"`
	TxDrops       int     `json:"tx_drops"`
}

type Telemetry struct {
	Type            string   `json:"type"`
	TMS             int64    `json:"t_ms"`
	CaptureID       string   `json:"capture_id,omitempty"`
	VADState        string   `json:"vad_state"`
	VADScore        int      `json:"vad_score"`
	RMSDBFS         *float64 `json:"rms_dbfs"`
	PeakDBFS        *float64 `json:"peak_dbfs"`
	NoiseDBFS       *float64 `json:"noise_dbfs"`
	GainDB          *float64 `json:"gain_db"`
	TxQueueMS       int      `json:"tx_queue_ms"`
	TxDrops         int      `json:"tx_drops"`
	TxErrors        int      `json:"tx_errors"`
	CaptureOverruns int      `json:"capture_overruns"`
	Reconnects      int      `json:"reconnects"`
	RSSIDBm         *int     `json:"rssi_dbm,omitempty"`
	HeapFree        *int     `json:"heap_free,omitempty"`
	UptimeMS        int64    `json:"uptime_ms"`
}

// ---------------------------------------------------------------------------
// Server -> device commands
// ---------------------------------------------------------------------------

type ServerCommand struct {
	Type string          `json:"type"`
	ID   int64           `json:"id,omitempty"`
	Raw  json.RawMessage `json:"-"`
}

type StartCapture struct {
	Type       string       `json:"type"`
	ID         int64        `json:"id,omitempty"`
	Mode       string       `json:"mode,omitempty"`
	Label      string       `json:"label,omitempty"`
	GainDB     *float64     `json:"gain_db,omitempty"`
	Audio      *AudioConfig `json:"audio,omitempty"`
	DurationMS int          `json:"duration_ms,omitempty"`
}

type Configure struct {
	Type      string           `json:"type"`
	ID        int64            `json:"id,omitempty"`
	Audio     *AudioConfig     `json:"audio,omitempty"`
	Capture   *CaptureConfig   `json:"capture,omitempty"`
	Telemetry *TelemetryConfig `json:"telemetry,omitempty"`
}

type SetGain struct {
	Type    string   `json:"type"`
	ID      int64    `json:"id,omitempty"`
	GainDB  *float64 `json:"gain_db,omitempty"`
	GainRaw *int     `json:"gain_raw,omitempty"`
}

type TestTone struct {
	Type          string  `json:"type"`
	ID            int64   `json:"id,omitempty"`
	ToneID        string  `json:"tone_id"`
	Waveform      string  `json:"waveform"`
	FrequencyHz   float64 `json:"frequency_hz,omitempty"`
	StartHz       float64 `json:"start_hz,omitempty"`
	EndHz         float64 `json:"end_hz,omitempty"`
	SweepMode     string  `json:"sweep_mode,omitempty"`
	AmplitudeDBFS float64 `json:"amplitude_dbfs"`
	DurationMS    int     `json:"duration_ms"`
	FadeMS        int     `json:"fade_ms,omitempty"`
	SpeakerVolume int     `json:"speaker_volume,omitempty"`
	Capture       bool    `json:"capture"`
	Loopback      bool    `json:"loopback"`
	Label         string  `json:"label,omitempty"`
}

// ---------------------------------------------------------------------------
// MQTT control envelopes (docs/mqtt-control.md)
// ---------------------------------------------------------------------------

// FlexID accepts a JSON string or number and normalizes it to a string.
type FlexID string

func (id *FlexID) UnmarshalJSON(data []byte) error {
	data = bytes.TrimSpace(data)
	if len(data) == 0 {
		return nil
	}
	if data[0] == '"' {
		var s string
		if err := json.Unmarshal(data, &s); err != nil {
			return err
		}
		*id = FlexID(s)
		return nil
	}
	var n json.Number
	if err := json.Unmarshal(data, &n); err != nil {
		return err
	}
	*id = FlexID(n.String())
	return nil
}

func (id FlexID) String() string { return string(id) }

type MqttCommand struct {
	V           int             `json:"v"`
	Type        string          `json:"type"`
	ID          FlexID          `json:"id"`
	Action      string          `json:"action"`
	IssuedAtMS  *int64          `json:"issued_at_ms,omitempty"`
	ExpiresAtMS *int64          `json:"expires_at_ms,omitempty"`
	Params      json.RawMessage `json:"params,omitempty"`
}

func ParseMqttCommand(data []byte) (*MqttCommand, error) {
	var c MqttCommand
	dec := json.NewDecoder(bytes.NewReader(data))
	if err := dec.Decode(&c); err != nil {
		return nil, fmt.Errorf("%s: %w", ErrBadMessage, err)
	}
	if c.V != 1 {
		return nil, fmt.Errorf("%s: envelope version %d", ErrUnsupportedVersion, c.V)
	}
	if c.Type != "recorder_cmd" {
		return nil, fmt.Errorf("%s: type %q", ErrBadMessage, c.Type)
	}
	if c.ID.String() == "" {
		return nil, fmt.Errorf("%s: missing id", ErrBadMessage)
	}
	switch c.Action {
	case "start", "stop", "configure", "status", "test_tone":
	default:
		return nil, fmt.Errorf("%s: action %q", ErrUnsupportedAction, c.Action)
	}
	return &c, nil
}

type MqttStartParams struct {
	Mode       string       `json:"mode,omitempty"`
	Label      string       `json:"label,omitempty"`
	DurationMS int          `json:"duration_ms,omitempty"`
	Audio      *AudioConfig `json:"audio,omitempty"`
	GainDB     *float64     `json:"gain_db,omitempty"`
	PreRollMS  int          `json:"pre_roll_ms,omitempty"`
	Restart    bool         `json:"restart,omitempty"`
}

type MqttStatus struct {
	V         int          `json:"v"`
	Type      string       `json:"type"`
	ID        string       `json:"id"`
	State     string       `json:"state"`
	CaptureID string       `json:"capture_id,omitempty"`
	SegmentID uint32       `json:"segment_id,omitempty"`
	Audio     *AudioConfig `json:"audio,omitempty"`
	GainDB    *float64     `json:"gain_db"`
	Queue     MqttQueue    `json:"queue"`
	Error     *MqttError   `json:"error"`
	TSMS      int64        `json:"ts_ms"`
}

type MqttQueue struct {
	TxMS       int `json:"tx_ms"`
	Drops      int `json:"drops"`
	Reconnects int `json:"reconnects"`
}

type MqttError struct {
	Code      string `json:"code"`
	Message   string `json:"message"`
	Retryable bool   `json:"retryable"`
}

// IDString is a small helper used by tests and tools.
func IDString(v any) string {
	switch x := v.(type) {
	case string:
		return x
	case float64:
		return strconv.FormatInt(int64(x), 10)
	case json.Number:
		return x.String()
	default:
		return fmt.Sprint(x)
	}
}
