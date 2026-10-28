#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include "rapidjson/document.h"

#include "cl_audio_sink.h"
#include "cl_audio_source.h"
#include "cl_mqtt_control.h"
#include "cl_opus_codec.h"
#include "cl_recorder_client.h"
#include "cl_recorder_commands.h"
#include "cl_recorder_messages.h"
#include "cl_recorder_segmenter.h"
#include "cl_recorder_tone.h"
#include "cl_recorder_types.h"

namespace recorder {

struct AppDependencies {
  AudioSource* source = nullptr;
  RecorderClient* client = nullptr;
  MqttControl* mqtt = nullptr;
  Clock* clock = nullptr;
  AudioSink* sink = nullptr;  // null: test_tone is not advertised / supported

  DeviceInfo device;
  Capabilities capabilities;
  ControlConfig control;
  AudioConfig audio;
  CaptureConfig capture;
  TelemetryConfig telemetry;
  Limits limits;

  std::string ws_url;
  std::string ws_token;
  std::string ws_ca_file;
  std::string ws_ca_pem;       // MCU builds carry the CA inline
  bool ws_allow_insecure = false;  // develop-only WSS without verification
  bool debug_allow_ws_control = false;
  std::string default_label;
};

struct StartParams {
  CommandSource source = CommandSource::kWs;
  CaptureMode mode = CaptureMode::kVad;
  bool has_mode = false;
  std::string label;
  int duration_ms = 0;
  bool has_duration = false;
  int pre_roll_ms = -1;  // -1 = keep the configured value
  bool has_pre_roll = false;
  bool restart = false;
  bool has_audio = false;
  AudioConfig audio;
  bool has_gain = false;
  int gain_db = 0;
};

class RecorderApp : public SegmentSink {
 public:
  explicit RecorderApp(AppDependencies deps);
  ~RecorderApp() override = default;

  bool Init();
  void Poll();
  void Shutdown();

  void OnSegmentStart(const SegmentStartEvent& ev) override;
  void OnSegmentFrame(const SegmentFrameEvent& ev) override;
  void OnSegmentEnd(const SegmentEndEvent& ev) override;

  RecorderState state() const { return state_; }
  bool capturing() const { return capture_active_; }

 private:
  void ProcessClientEvents();
  void ProcessMqttCommands();
  void ProcessAudio();
  void ProcessTimers();
  void ProcessTone();

  void HandleClientEvent(const ClientEvent& ev);
  void HandleWsCommand(const std::string& text);
  void HandleMqttEnvelope(const std::string& text);
  void HandleHelloAck(const rapidjson::Document& doc);

  bool ConnectWs();
  void SendHello();
  void SendCaptureState(const std::string& state);
  void SendTelemetry();
  std::string PublishMqttStatus(const std::string& id, const std::string& error_code = "",
                                const std::string& error_message = "", bool retryable = false);

  bool StartCapture(const StartParams& params, std::string* error_code,
                    std::string* error_message);
  void StopCapture(const std::string& reason, const std::string& error_code = "",
                   const std::string& status_id = "");
  void SetState(RecorderState state);
  void ScheduleReconnect();

  bool ValidateAudio(const AudioConfig& audio, std::string* code, std::string* message) const;
  bool ApplyAudio(const AudioConfig& audio, std::string* code, std::string* message);
  bool EnsureHandshake(std::string* code, std::string* message);

  bool StartParamsFromMqtt(const MqttCommand& cmd, StartParams* out,
                           std::string* code, std::string* message);
  bool StartParamsFromWs(const WsCommand& cmd, StartParams* out,
                         std::string* code, std::string* message);

  void HandleWsTestTone(const WsCommand& cmd);
  void StartTone(const ToneMetadata& tone, bool capture, bool ws_command, bool has_id,
                 int64_t id, const std::string& mqtt_id, const std::string& label);
  void FinishTone(bool success, const std::string& error_code = "");

  void RememberWsResponse(int64_t id, const std::string& json);
  bool FindWsResponse(int64_t id, std::string* json) const;
  void RememberMqttStatus(const std::string& id, const std::string& json);
  bool FindMqttStatus(const std::string& id, std::string* json) const;

  StatsSnapshot MakeStats() const;
  void ResetStats();

  AppDependencies deps_;
  AudioSource* source_ = nullptr;
  RecorderClient* client_ = nullptr;
  MqttControl* mqtt_ = nullptr;
  Clock* clock_ = nullptr;

  RecorderState state_ = RecorderState::kIdle;
  bool recording_intent_ = false;
  bool capture_active_ = false;
  bool hello_sent_ = false;
  bool hello_done_ = false;
  bool source_configured_ = false;
  AudioConfig configured_audio_;

  // Effective configuration (device defaults overridden by hello_ack/commands).
  AudioConfig audio_;
  CaptureConfig capture_;
  TelemetryConfig telemetry_;
  Limits limits_;
  ControlConfig control_;
  std::string device_id_;

  std::string capture_id_;
  uint32_t current_segment_id_ = 0;
  uint64_t segment_drop_baseline_ = 0;

  OpusEncoder encoder_;
  RecorderSegmenter segmenter_;
  std::vector<uint8_t> encoded_;

  int64_t capture_start_ms_ = 0;
  int64_t capture_start_us_ = 0;
  int64_t telemetry_last_ms_ = 0;
  int64_t stats_last_ms_ = 0;
  int64_t drain_deadline_ms_ = 0;
  int64_t next_reconnect_ms_ = 0;
  int64_t reconnect_backoff_ms_ = 1000;
  int64_t mqtt_lost_since_ms_ = 0;
  bool mqtt_connected_ = false;
  bool mqtt_saw_connected_ = false;
  std::string last_error_code_;

  bool fatal_control_overflow_ = false;

  // A start that arrived before the WS handshake completed.
  StartParams pending_start_;
  bool has_pending_start_ = false;
  std::string pending_mqtt_id_;

  // Test tone state.
  bool tone_active_ = false;
  bool tone_start_sent_ = false;
  bool tone_ws_command_ = false;
  bool tone_has_id_ = false;
  int64_t tone_command_id_ = 0;
  std::string tone_mqtt_id_;
  bool tone_capture_ = false;
  ToneMetadata tone_meta_;
  ToneGenerator tone_gen_;
  std::vector<int16_t> tone_buf_;
  std::size_t tone_buf_pos_ = 0;
  std::size_t tone_buf_frames_ = 0;
  int64_t tone_start_ms_ = 0;
  bool tone_speaker_ready_ = false;

  RecorderStats counters_;

  std::deque<int64_t> ws_dedup_order_;
  std::unordered_map<int64_t, std::string> ws_dedup_;
  std::deque<std::string> mqtt_dedup_order_;
  std::unordered_map<std::string, std::string> mqtt_dedup_;
};

}  // namespace recorder
