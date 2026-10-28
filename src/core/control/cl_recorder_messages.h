#pragma once

#include <cstdint>
#include <string>

#include "cl_recorder_segmenter.h"
#include "cl_recorder_types.h"

namespace recorder {

struct ClockSnapshot {
  int64_t monotonic_ms = 0;
  int64_t wall_ms = 0;
  bool synced = false;
  int64_t capture_t0_mono_us = 0;
};

struct HelloContext {
  DeviceInfo device;
  Capabilities capabilities;
  ControlConfig control;
  ClockSnapshot clock;
};

std::string BuildHello(const HelloContext& ctx);

// `audio`/`capture` may be null when the device has not applied a config yet.
std::string BuildCaptureState(const std::string& state, const std::string& capture_id,
                              const std::string& mode, const AudioConfig* audio,
                              const CaptureConfig* capture, bool gain_supported, int gain_db,
                              int tx_queue_ms, int tx_drops, int tx_errors, int reconnects);

std::string BuildSegmentStart(const SegmentStartEvent& ev, const std::string& capture_id,
                              const AudioConfig& audio, bool gain_supported, int gain_db);
std::string BuildSegmentEnd(const SegmentEndEvent& ev, const std::string& capture_id);

struct TelemetrySnapshot {
  int64_t t_ms = 0;
  std::string capture_id;
  std::string vad_state = "unknown";
  int vad_score = protocol::kVadScoreUnknown;
  bool has_levels = false;
  double rms_dbfs = -200.0;
  double peak_dbfs = -200.0;
  double noise_dbfs = -200.0;
  bool has_gain = false;
  double gain_db = 0.0;
  int tx_queue_ms = 0;
  int tx_drops = 0;
  int tx_errors = 0;
  int capture_overruns = 0;
  int reconnects = 0;
  bool has_rssi = false;
  int rssi_dbm = 0;
  bool has_heap = false;
  int64_t heap_free = 0;
  int64_t uptime_ms = 0;
};

std::string BuildTelemetry(const TelemetrySnapshot& s);

std::string BuildAck(bool has_id, int64_t id, const std::string& result_json = "{}");
std::string BuildNack(bool has_id, int64_t id, const std::string& code,
                      const std::string& message, bool retryable = false);
std::string BuildAccepted(bool has_id, int64_t id, const std::string& detail);
std::string BuildCompleted(bool has_id, int64_t id, const std::string& result_json = "{}");
std::string BuildFailed(bool has_id, int64_t id, const std::string& code,
                        const std::string& detail);
std::string BuildErrorJson(const std::string& code, const std::string& message, bool fatal);
std::string BuildPong(bool has_id, int64_t id, int64_t t0_ms, int64_t t1_ms,
                      bool has_server_time, int64_t server_time_ms);
std::string BuildToneStart(const std::string& tone_id, uint32_t t_ms, uint32_t segment_id);
std::string BuildToneEnd(const std::string& tone_id, uint32_t t_ms, uint32_t segment_id,
                         const std::string& reason);

struct MqttStatusData {
  RecorderState state = RecorderState::kIdle;
  std::string capture_id;
  uint32_t segment_id = 0;
  bool has_audio = false;
  AudioConfig audio;
  bool gain_supported = false;
  double gain_db = 0.0;
  int tx_ms = 0;
  int drops = 0;
  int reconnects = 0;
  bool has_error = false;
  std::string error_code;
  std::string error_message;
  bool error_retryable = false;
  int64_t ts_ms = 0;
};

std::string BuildMqttStatus(const std::string& id, const MqttStatusData& data);

struct StatsSnapshot {
  RecorderStats counters;
  int tx_queue_ms = 0;
  uint64_t tx_queue_records = 0;
  bool has_audio = false;
  AudioConfig audio;
  std::string capture_id;
  std::string state;
};

std::string BuildStatsJson(const StatsSnapshot& s);
std::string BuildStatusJson(const StatsSnapshot& s);

}  // namespace recorder
