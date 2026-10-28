#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cl_recorder_protocol.h"

namespace recorder {

// Recorder-local result type. The recorder does not depend on ai-voice; this
// mirrors the small error vocabulary the rest of the code uses.
enum class AvpErr : uint8_t {
  kOk = 0,
  kErrFail,
  kErrInvalidArg,
  kErrNoMem,
  kErrTimeout,
  kErrNotInit,
  kErrAlreadyInit,
  kErrNotConnected,
  kErrBusy,
  kErrNotSupported,
};

enum class CaptureMode : uint8_t { kVad, kContinuous, kSingle };
enum class VadEngine : uint8_t { kAuto, kEnergy, kDevice, kServer };
enum class RecorderState : uint8_t { kIdle, kConnectingWs, kHandshaking, kCapturing, kDraining, kError };
enum class CommandSource : uint8_t { kWs, kMqtt };

const char* CaptureModeName(CaptureMode m);
const char* VadEngineName(VadEngine e);
const char* RecorderStateName(RecorderState s);
bool ParseCaptureMode(const std::string& s, CaptureMode* out);
bool ParseVadEngine(const std::string& s, VadEngine* out);
bool ParseAudioFormatName(const std::string& s, AudioFormat* out);

// Negotiated audio configuration (docs/protocol.md §5.3).
struct AudioConfig {
  AudioFormat format = AudioFormat::kPcmS16le;
  int sample_rate = 16000;
  int channels = 1;
  int frame_ms = 20;
  int bits_per_sample = 16;
  bool interleaved = true;
  // Opus-only, host encoders. Zero/empty means "use defaults".
  int opus_bitrate = 0;
  int opus_complexity = -1;
  bool opus_vbr = true;
  std::string opus_application;
  std::string opus_encoder;

  int FrameSamples() const { return sample_rate * frame_ms / 1000; }
  std::size_t PcmBytesPerFrame() const {
    return static_cast<std::size_t>(FrameSamples()) * static_cast<std::size_t>(channels) * 2;
  }
  std::size_t PayloadMax() const {
    return format == AudioFormat::kPcmS16le ? protocol::kMaxPcmPayload : protocol::kMaxOpusPayload;
  }
  int BytesPerMs() const {
    return format == AudioFormat::kPcmS16le ? sample_rate * channels * 2 / 1000 : 0;
  }
  bool operator==(const AudioConfig& o) const {
    return format == o.format && sample_rate == o.sample_rate && channels == o.channels &&
           frame_ms == o.frame_ms && bits_per_sample == o.bits_per_sample &&
           interleaved == o.interleaved && opus_bitrate == o.opus_bitrate &&
           opus_complexity == o.opus_complexity && opus_vbr == o.opus_vbr &&
           opus_application == o.opus_application && opus_encoder == o.opus_encoder;
  }
  bool operator!=(const AudioConfig& o) const { return !(*this == o); }
};

// VAD parameters (docs/protocol.md §5.4).
struct VadConfig {
  VadEngine engine = VadEngine::kAuto;
  int onset_ms = 120;
  int silence_ms = 700;
  int min_speech_ms = 150;
  int guard_ms = 0;
  double threshold_db = 6.0;
  bool send_short_segments = false;
};

// Capture parameters.
struct CaptureConfig {
  CaptureMode mode = CaptureMode::kVad;
  int pre_roll_ms = 300;
  int max_segment_ms = 15000;
  int segment_ms = 10000;
  int duration_ms = 0;
  VadConfig vad;
  bool gain_supported = false;
  int gain_db = 0;
};

struct TelemetryConfig {
  bool frame_stats = true;
  int level_interval_ms = 500;
  int stats_interval_ms = 5000;
};

struct Limits {
  int max_frame_bytes = 8192;
  int max_queue_ms = 3000;
};

// MQTT/WS control ownership (docs/protocol.md §4.2).
struct ControlConfig {
  std::string owner = "mqtt";
  bool allow_ws_capture_control = false;
  std::string ws_mode = "on_demand";
  int mqtt_lost_stop_timeout_ms = 30000;
  int max_capture_ms = 1800000;
  int ws_idle_timeout_ms = 10000;
};

struct DeviceInfo {
  std::string id = "recorder-linux";
  std::string model = "linux-pcm";
  std::string firmware = "0.1.0";
  std::string chip = "linux";
  std::string audio_frontend = "miniaudio";
  std::string boot_id;
};

// Advertised capabilities. Only verified facts belong here.
struct FormatCapability {
  std::vector<int> sample_rates;
  std::vector<int> channels;
  std::vector<int> frame_ms;
  std::vector<int> bitrate;
  int payload_max = 0;
  std::vector<std::string> encoders;
  std::vector<std::string> channel_map;
};

struct Capabilities {
  bool pcm_s16le = false;
  FormatCapability pcm;
  bool opus = false;
  FormatCapability opus_cap;
  std::vector<std::string> vad;  // "server", "energy", "device"
  bool gain_supported = false;
  int gain_min = 0;
  int gain_max = 0;
  int gain_step = 1;
  std::string gain_unit = "dB";
  bool test_tone = false;
  std::vector<std::string> tone_waveforms;
  double tone_min_hz = 20.0;
  double tone_max_hz = 20000.0;
  double tone_max_amplitude_dbfs = 0.0;
  bool speaker = false;
  int speaker_min = 0;
  int speaker_max = 100;
  bool loopback = false;
  int device_pre_roll_max_ms = 5000;
  bool server_pre_roll = true;
  int max_segment_ms = 30000;
  int tx_queue_ms = 3000;
  int max_frame_bytes = 8192;

  bool SupportsFormat(AudioFormat f) const {
    return (f == AudioFormat::kPcmS16le && pcm_s16le) || (f == AudioFormat::kOpus && opus);
  }
};

// Per-frame statistics computed from PCM (docs/audio-quality-metrics.md §2).
struct FrameStats {
  bool valid = false;
  double rms_dbfs = -200.0;
  int16_t rms_dbfs_x100 = protocol::kStatUnavailableI16;
  uint16_t peak = protocol::kStatUnavailableU16;
  int16_t dc_offset = protocol::kStatUnavailableI16;
  bool clipped = false;
  uint8_t vad_score = protocol::kVadScoreUnknown;
};

// One captured frame as returned by an AudioSource. `data`/`pcm` point into
// source-owned storage valid until the next Read().
// Test-tone stimulus metadata echoed in segment_start.tone.
struct ToneMetadata {
  std::string tone_id;
  std::string waveform = "sine";
  double frequency_hz = 0.0;
  double start_hz = 0.0;
  double end_hz = 0.0;
  std::string sweep_mode;
  double amplitude_dbfs = -6.0;
  int speaker_volume = 0;
  std::string capture_path = "mic";
  bool loopback = false;
  int fade_ms = 20;
  int duration_ms = 0;
};

struct CaptureFrame {
  const uint8_t* data = nullptr;
  std::size_t bytes = 0;
  std::size_t samples = 0;  // per channel
  uint32_t timestamp_ms = 0;
  AudioFormat format = AudioFormat::kPcmS16le;
  const int16_t* pcm = nullptr;
  std::size_t pcm_samples = 0;  // per channel
  bool stats_valid = false;
  FrameStats stats;
  bool discontinuity = false;
  bool capture_overrun = false;
};

struct SourceStats {
  uint64_t frames = 0;
  uint64_t overruns = 0;
  uint64_t dropped = 0;
  uint64_t errors = 0;
};

// Lifetime counters reported by stats/status.
struct RecorderStats {
  uint64_t frames_produced = 0;
  uint64_t frames_sent = 0;
  uint64_t frames_dropped = 0;
  uint64_t bytes_sent = 0;
  uint64_t send_errors = 0;
  uint64_t capture_overruns = 0;
  uint64_t reconnects = 0;
  uint64_t segments_produced = 0;
  uint64_t segments_discarded = 0;
  uint64_t tx_queue_high_water = 0;
};

// Monotonic and wall clock helpers supplied by the platform.
struct Clock {
  virtual ~Clock() = default;
  virtual int64_t MonotonicMs() const = 0;
  virtual int64_t MonotonicUs() const = 0;
  virtual int64_t WallMs() const = 0;
};

}  // namespace recorder
