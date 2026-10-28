#include "cl_recorder_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "cl_recorder_messages.h"
#include "utils/cl_logger.h"

namespace recorder {

namespace {

const char* kTag = "recorder";
constexpr int64_t kMinReconnectMs = 1000;
constexpr int64_t kMaxReconnectMs = 30000;
constexpr std::size_t kMaxDedup = 32;
constexpr int kToneSampleRate = 16000;
constexpr int kToneChannels = 1;
constexpr std::size_t kToneChunkFrames = 160;  // 10 ms at 16 kHz

bool ListHas(const std::vector<int>& v, int x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}
bool ListHas(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

std::string ErrorCodeFromMessage(const std::string& message) {
  const std::size_t colon = message.find(':');
  return colon == std::string::npos ? message : message.substr(0, colon);
}

}  // namespace

RecorderApp::RecorderApp(AppDependencies deps) : deps_(std::move(deps)) {
  source_ = deps_.source;
  client_ = deps_.client;
  mqtt_ = deps_.mqtt;
  clock_ = deps_.clock;

  audio_ = deps_.audio;
  capture_ = deps_.capture;
  telemetry_ = deps_.telemetry;
  limits_ = deps_.limits;
  control_ = deps_.control;
  if (control_.owner.empty()) control_.owner = "mqtt";
  if (deps_.debug_allow_ws_control) control_.allow_ws_capture_control = true;
  device_id_ = deps_.device.id;
  if (deps_.device.boot_id.empty()) {
    char boot[16];
    std::snprintf(boot, sizeof(boot), "%08x",
                  static_cast<unsigned>(deps_.clock != nullptr ? deps_.clock->MonotonicMs() : 0) ^
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(this)));
    deps_.device.boot_id = boot;
  }
  if (client_ != nullptr) client_->SetClock(clock_);
  segmenter_.SetSink(this);
  tone_buf_.assign(kToneChunkFrames * kToneChannels, 0);
}

bool RecorderApp::Init() {
  if (source_ == nullptr || client_ == nullptr || clock_ == nullptr) {
    LOG_E(kTag, "missing AudioSource/RecorderClient/Clock");
    return false;
  }
  if (mqtt_ != nullptr && !mqtt_->Init()) {
    LOG_E(kTag, "MQTT control init failed");
    return false;
  }
  if (control_.ws_mode == "always") {
    SetState(RecorderState::kConnectingWs);
    if (!ConnectWs()) ScheduleReconnect();
  }
  return true;
}

void RecorderApp::SetState(RecorderState state) {
  if (state_ == state) return;
  state_ = state;
  LOG_I(kTag, "state -> %s", RecorderStateName(state_));
}

void RecorderApp::Shutdown() {
  if (capture_active_) StopCapture("stop_capture");
  client_->Flush();
  client_->Disconnect();
  SetState(RecorderState::kIdle);
}

void RecorderApp::Poll() {
  ProcessClientEvents();
  ProcessMqttCommands();

  if (fatal_control_overflow_) {
    fatal_control_overflow_ = false;
    if (capture_active_) StopCapture("queue_overflow", "queue_overflow");
  }

  ProcessTimers();

  if (capture_active_) {
    ProcessAudio();
    if (segmenter_.TakeCaptureComplete()) StopCapture("max_duration");
  }

  ProcessTone();
  ProcessTimers();
  client_->Flush();
}

void RecorderApp::ProcessClientEvents() {
  ClientEvent ev;
  for (int i = 0; i < 32 && client_->TakeEvent(&ev); ++i) {
    HandleClientEvent(ev);
  }
}

void RecorderApp::HandleClientEvent(const ClientEvent& ev) {
  switch (ev.type) {
    case ClientEventType::kConnected: {
      reconnect_backoff_ms_ = kMinReconnectMs;
      next_reconnect_ms_ = 0;
      hello_done_ = false;
      SendHello();
      break;
    }
    case ClientEventType::kDisconnected: {
      const bool was_capturing = capture_active_;
      if (capture_active_) {
        // Close locally; the server finalizes the partial capture and records
        // transport_lost. Stale records must not leak into the next session.
        segmenter_.EndCapture("transport_lost");
        source_->Stop();
        capture_active_ = false;
        counters_.segments_produced = static_cast<uint64_t>(segmenter_.segments_produced());
        counters_.segments_discarded = static_cast<uint64_t>(segmenter_.segments_discarded());
      }
      client_->ClearAll();
      capture_id_.clear();
      hello_sent_ = false;
      hello_done_ = false;
      ++counters_.reconnects;
      if (was_capturing) {
        last_error_code_ = "ws_unavailable";
        PublishMqttStatus("", "ws_unavailable", "websocket lost", true);
      }
      if (recording_intent_) {
        SetState(RecorderState::kConnectingWs);
        ScheduleReconnect();
      } else {
        SetState(RecorderState::kIdle);
      }
      break;
    }
    case ClientEventType::kText:
      HandleWsCommand(ev.text);
      break;
    case ClientEventType::kError:
      LOG_W(kTag, "ws: %s", ev.text.c_str());
      break;
    case ClientEventType::kControlQueueFull:
      fatal_control_overflow_ = true;
      break;
  }
}

void RecorderApp::SendHello() {
  if (!client_->IsConnected()) return;
  HelloContext ctx;
  ctx.device = deps_.device;
  ctx.capabilities = deps_.capabilities;
  ctx.control = control_;
  ctx.clock.monotonic_ms = clock_->MonotonicMs();
  ctx.clock.wall_ms = clock_->WallMs();
  ctx.clock.synced = true;
  ctx.clock.capture_t0_mono_us = capture_active_ ? capture_start_us_ : 0;
  const std::string hello = BuildHello(ctx);
  if (!client_->EnqueueControl(hello)) fatal_control_overflow_ = true;
  hello_sent_ = true;
  SetState(RecorderState::kHandshaking);
}

bool RecorderApp::ConnectWs() {
  if (client_->IsConnected()) return true;
  if (deps_.ws_url.empty()) {
    LOG_E(kTag, "no WebSocket URL configured");
    ScheduleReconnect();
    return false;
  }
  WsConfig cfg;
  cfg.url = deps_.ws_url;
  cfg.ca_file = deps_.ws_ca_file;
  cfg.ca_pem = deps_.ws_ca_pem;
  cfg.allow_insecure = deps_.ws_allow_insecure;
  if (!deps_.ws_token.empty()) {
    cfg.headers.emplace_back("Authorization", "Bearer " + deps_.ws_token);
  }
  cfg.headers.emplace_back("Device-Id", device_id_);
  cfg.headers.emplace_back("Protocol-Version", "1");
  if (!client_->Connect(cfg)) {
    LOG_W(kTag, "WebSocket connect failed");
    SetState(RecorderState::kConnectingWs);
    ScheduleReconnect();
    return false;
  }
  SetState(RecorderState::kConnectingWs);
  // Connection watchdog: if neither connected nor disconnected fires, retry
  // after a bounded wait rather than sitting in connecting_ws forever.
  next_reconnect_ms_ = clock_->MonotonicMs() + 15000;
  return true;
}

void RecorderApp::ScheduleReconnect() {
  const int64_t now = clock_->MonotonicMs();
  next_reconnect_ms_ = now + reconnect_backoff_ms_ + (now % 500);
  reconnect_backoff_ms_ = std::min<int64_t>(kMaxReconnectMs,
                                            std::max<int64_t>(kMinReconnectMs,
                                                              reconnect_backoff_ms_ * 2));
}

bool RecorderApp::ValidateAudio(const AudioConfig& audio, std::string* code,
                                std::string* message) const {
  const Capabilities& caps = deps_.capabilities;
  if (!caps.SupportsFormat(audio.format)) {
    *code = "unsupported_format";
    *message = std::string("format not advertised: ") + AudioFormatName(audio.format);
    return false;
  }
  if (audio.sample_rate <= 0 || audio.channels <= 0 || audio.frame_ms <= 0) {
    *code = "unsupported_config";
    *message = "invalid rate/channels/frame_ms";
    return false;
  }
  if ((audio.sample_rate * audio.frame_ms) % 1000 != 0) {
    *code = "unsupported_config";
    *message = "frame duration is not an exact sample count";
    return false;
  }

  if (audio.format == AudioFormat::kPcmS16le) {
    if (!ListHas(caps.pcm.sample_rates, audio.sample_rate) ||
        !ListHas(caps.pcm.channels, audio.channels) ||
        !ListHas(caps.pcm.frame_ms, audio.frame_ms)) {
      *code = "unsupported_format";
      *message = "PCM rate/channels/frame not advertised";
      return false;
    }
    const std::size_t payload = audio.PcmBytesPerFrame();
    if (payload == 0 || payload > protocol::kMaxPcmPayload ||
        payload > static_cast<std::size_t>(caps.pcm.payload_max)) {
      *code = "unsupported_format";
      *message = "PCM frame payload exceeds the advertised maximum";
      return false;
    }
  } else {
    if (!caps.opus) {
      *code = "unsupported_format";
      *message = "opus not advertised";
      return false;
    }
    if (!ListHas(caps.opus_cap.sample_rates, audio.sample_rate) ||
        !ListHas(caps.opus_cap.channels, audio.channels) ||
        !ListHas(caps.opus_cap.frame_ms, audio.frame_ms)) {
      *code = "unsupported_format";
      *message = "Opus rate/channels/frame not advertised";
      return false;
    }
    if (!audio.opus_encoder.empty() && !caps.opus_cap.encoders.empty() &&
        !ListHas(caps.opus_cap.encoders, audio.opus_encoder)) {
      *code = "unsupported_format";
      *message = "Opus encoder not advertised: " + audio.opus_encoder;
      return false;
    }
  }
  return true;
}

bool RecorderApp::ApplyAudio(const AudioConfig& audio, std::string* code,
                             std::string* message) {
  AudioConfig next = audio;
  if (next.format == AudioFormat::kOpus && next.opus_encoder.empty()) {
    next.opus_encoder = source_->NativeFormat() == AudioFormat::kPcmS16le ? "host" : "gx8006";
  }
  if (next.format == AudioFormat::kPcmS16le &&
      source_->NativeFormat() != AudioFormat::kPcmS16le) {
    *code = "unsupported_format";
    *message = "source cannot produce PCM";
    return false;
  }
  if (!ValidateAudio(next, code, message)) return false;

  audio_ = next;
  if (source_configured_ && configured_audio_ == audio_) return true;

  const AvpErr err = source_->Configure(audio_);
  if (err != AvpErr::kOk) {
    *code = "audio_hw_error";
    *message = "failed to configure the audio source";
    return false;
  }
  if (audio_.format == AudioFormat::kOpus &&
      source_->NativeFormat() == AudioFormat::kPcmS16le) {
    if (encoder_.Init(audio_) != AvpErr::kOk) {
      *code = "unsupported_config";
      *message = "host Opus encoder init failed";
      return false;
    }
  }
  configured_audio_ = audio_;
  source_configured_ = true;
  return true;
}

bool RecorderApp::StartParamsFromMqtt(const MqttCommand& cmd, StartParams* out,
                                      std::string* code, std::string* message) {
  *out = StartParams{};
  out->source = CommandSource::kMqtt;
  out->mode = capture_.mode;
  const rapidjson::Value* p = cmd.Params();
  if (p == nullptr) return true;

  std::string s;
  int v = 0;
  double d = 0;
  if (JsonGetString(*p, "mode", &s)) {
    if (!ParseCaptureMode(s, &out->mode)) {
      *code = "unsupported_config";
      *message = "unsupported mode: " + s;
      return false;
    }
    out->has_mode = true;
  }
  if (JsonGetString(*p, "label", &s)) out->label = s;
  if (JsonGetInt(*p, "duration_ms", &v)) {
    out->duration_ms = v;
    out->has_duration = true;
  }
  if (JsonGetInt(*p, "pre_roll_ms", &v)) {
    out->pre_roll_ms = v;
    out->has_pre_roll = true;
  }
  if (JsonGetDouble(*p, "gain_db", &d)) {
    out->has_gain = true;
    out->gain_db = static_cast<int>(std::lround(d));
  }
  bool restart = false;
  if (JsonGetBool(*p, "restart", &restart)) out->restart = restart;
  if (p->HasMember("audio") && (*p)["audio"].IsObject()) {
    AudioConfig a = audio_;
    std::string err;
    if (!ParseAudioConfigJson((*p)["audio"], &a, &err)) {
      *code = ErrorCodeFromMessage(err);
      *message = err;
      return false;
    }
    out->audio = a;
    out->has_audio = true;
  }
  return true;
}

bool RecorderApp::StartParamsFromWs(const WsCommand& cmd, StartParams* out,
                                    std::string* code, std::string* message) {
  *out = StartParams{};
  out->source = CommandSource::kWs;
  out->mode = capture_.mode;

  std::string s;
  int v = 0;
  double d = 0;
  if (JsonGetString(cmd.doc, "mode", &s)) {
    if (!ParseCaptureMode(s, &out->mode)) {
      *code = "unsupported_config";
      *message = "unsupported mode: " + s;
      return false;
    }
    out->has_mode = true;
  }
  if (JsonGetString(cmd.doc, "label", &s)) out->label = s;
  if (JsonGetInt(cmd.doc, "duration_ms", &v)) {
    out->duration_ms = v;
    out->has_duration = true;
  }
  if (JsonGetInt(cmd.doc, "pre_roll_ms", &v)) {
    out->pre_roll_ms = v;
    out->has_pre_roll = true;
  }
  if (JsonGetDouble(cmd.doc, "gain_db", &d)) {
    out->has_gain = true;
    out->gain_db = static_cast<int>(std::lround(d));
  }
  if (cmd.doc.HasMember("audio") && cmd.doc["audio"].IsObject()) {
    AudioConfig a = audio_;
    std::string err;
    if (!ParseAudioConfigJson(cmd.doc["audio"], &a, &err)) {
      *code = ErrorCodeFromMessage(err);
      *message = err;
      return false;
    }
    out->audio = a;
    out->has_audio = true;
  }
  return true;
}

bool RecorderApp::StartCapture(const StartParams& params, std::string* code,
                               std::string* message) {
  if (capture_active_) {
    if (!params.restart) {
      SendCaptureState("capturing");
      return true;
    }
    // A restart is a new capture. The capture_id is assigned by hello_ack, so
    // re-handshake rather than reuse the old one.
    StopCapture("stop_capture");
    client_->Disconnect();
    hello_done_ = false;
    hello_sent_ = false;
    capture_id_.clear();
    recording_intent_ = true;
    pending_start_ = params;
    has_pending_start_ = true;
    ConnectWs();
    return true;
  }

  if (!hello_done_ || !client_->IsConnected() || capture_id_.empty()) {
    recording_intent_ = true;
    pending_start_ = params;
    has_pending_start_ = true;
    if (!client_->IsConnected()) {
      SetState(RecorderState::kConnectingWs);
      ConnectWs();
    }
    return true;
  }

  if (params.has_mode) capture_.mode = params.mode;
  if (params.has_duration) capture_.duration_ms = params.duration_ms;
  if (params.has_pre_roll) {
    capture_.pre_roll_ms = std::max(0, params.pre_roll_ms);
  }
  if (capture_.pre_roll_ms > deps_.capabilities.device_pre_roll_max_ms) {
    capture_.pre_roll_ms = deps_.capabilities.device_pre_roll_max_ms;
  }
  if (params.has_gain && deps_.capabilities.gain_supported) {
    capture_.gain_db = params.gain_db;
  }
  if (capture_.mode == CaptureMode::kSingle && capture_.duration_ms <= 0) {
    *code = "unsupported_config";
    *message = "single mode requires duration_ms";
    return false;
  }
  if (capture_.vad.engine == VadEngine::kDevice &&
      !ListHas(deps_.capabilities.vad, "device")) {
    *code = "unsupported_config";
    *message = "device VAD is not available on this platform";
    return false;
  }
  // The WS hello_ack configuration is authoritative. An MQTT start may carry
  // audio overrides, but they must not silently replace what the data-plane
  // server negotiated.
  if (params.has_audio && params.source == CommandSource::kWs) {
    std::string c;
    std::string m;
    if (!ValidateAudio(params.audio, &c, &m)) {
      *code = c;
      *message = m;
      return false;
    }
    audio_ = params.audio;
    source_configured_ = false;
  } else if (params.has_audio && params.audio != audio_) {
    LOG_W(kTag, "ignoring MQTT start audio override; WS hello_ack wins");
  }

  if (!ApplyAudio(audio_, code, message)) return false;

  if (deps_.capabilities.gain_supported) {
    const AvpErr gain_err = source_->SetGain(capture_.gain_db);
    if (gain_err != AvpErr::kOk && gain_err != AvpErr::kErrNotSupported) {
      *code = "audio_hw_error";
      *message = "failed to apply microphone gain";
      return false;
    }
  }

  const std::string label = !params.label.empty() ? params.label : deps_.default_label;
  const int gain = deps_.capabilities.gain_supported ? capture_.gain_db : 0;
  segmenter_.Configure(audio_, capture_, label, deps_.capabilities.gain_supported, gain);

  if (source_->Start() != AvpErr::kOk) {
    *code = "audio_hw_error";
    *message = "failed to start the audio source";
    return false;
  }
  segmenter_.BeginCapture();
  capture_active_ = true;
  capture_start_ms_ = clock_->MonotonicMs();
  capture_start_us_ = clock_->MonotonicUs();
  telemetry_last_ms_ = capture_start_ms_;
  stats_last_ms_ = capture_start_ms_;
  recording_intent_ = true;
  has_pending_start_ = false;
  SetState(RecorderState::kCapturing);

  SendCaptureState("capturing");
  const std::string status = PublishMqttStatus(pending_mqtt_id_);
  if (!pending_mqtt_id_.empty()) RememberMqttStatus(pending_mqtt_id_, status);
  pending_mqtt_id_.clear();
  LOG_I(kTag, "capture started (capture_id=%s, %s, %d Hz, %d ch, %d ms)", capture_id_.c_str(),
        AudioFormatName(audio_.format), audio_.sample_rate, audio_.channels, audio_.frame_ms);
  return true;
}

void RecorderApp::StopCapture(const std::string& reason, const std::string& error_code,
                              const std::string& status_id) {
  recording_intent_ = false;
  has_pending_start_ = false;

  if (!capture_active_) {
    SetState(RecorderState::kIdle);
    if (hello_done_) SendCaptureState("idle");
    const std::string status =
        PublishMqttStatus(status_id, error_code, error_code.empty() ? reason : error_code, false);
    if (!status_id.empty()) RememberMqttStatus(status_id, status);
    return;
  }

  segmenter_.EndCapture(reason);
  source_->Stop();
  capture_active_ = false;
  counters_.segments_produced = static_cast<uint64_t>(segmenter_.segments_produced());
  counters_.segments_discarded = static_cast<uint64_t>(segmenter_.segments_discarded());

  if (hello_done_) SendCaptureState("idle");

  if (control_.ws_mode == "on_demand" && client_->IsConnected()) {
    drain_deadline_ms_ = clock_->MonotonicMs() + std::max(0, control_.ws_idle_timeout_ms);
    SetState(RecorderState::kDraining);
  } else {
    SetState(RecorderState::kIdle);
  }

  last_error_code_ = error_code;
  const std::string status =
      PublishMqttStatus(status_id, error_code, error_code.empty() ? reason : error_code, false);
  if (!status_id.empty()) RememberMqttStatus(status_id, status);
  LOG_I(kTag, "capture stopped (%s)", reason.c_str());
}

void RecorderApp::OnSegmentStart(const SegmentStartEvent& ev) {
  if (!capture_active_ || !hello_done_) return;
  current_segment_id_ = ev.segment_id;
  segment_drop_baseline_ = client_->queue().Stats().audio_dropped;

  if (ev.has_tone && !tone_start_sent_) {
    const std::string tone_start = BuildToneStart(ev.tone.tone_id, ev.t_ms, ev.segment_id);
    if (!client_->EnqueueControl(tone_start)) fatal_control_overflow_ = true;
    tone_start_sent_ = true;
  }

  const std::string json = BuildSegmentStart(ev, capture_id_, audio_,
                                             deps_.capabilities.gain_supported,
                                             capture_.gain_db);
  if (!client_->EnqueueControl(json)) fatal_control_overflow_ = true;
  counters_.segments_produced = static_cast<uint64_t>(segmenter_.segments_produced());
}

void RecorderApp::OnSegmentFrame(const SegmentFrameEvent& ev) {
  if (!capture_active_) return;
  TxRecord rec;
  rec.kind = TxRecord::Kind::kAudio;
  rec.format = ev.format;
  rec.segment_id = ev.segment_id;
  rec.seq = ev.seq;
  rec.timestamp_ms = ev.timestamp_ms;
  rec.samples = ev.samples;
  rec.frame_ms = static_cast<uint16_t>(audio_.frame_ms);
  rec.flags = ev.flags;
  if (ev.payload != nullptr && ev.payload_len > 0) {
    rec.payload.assign(ev.payload, ev.payload + ev.payload_len);
  }
  if (ev.stats.valid) {
    rec.rms_dbfs_x100 = ev.stats.rms_dbfs_x100;
    rec.peak = ev.stats.peak;
    rec.dc_offset = ev.stats.dc_offset;
    rec.vad_score = ev.stats.vad_score;
  }
  if (!client_->EnqueueAudio(std::move(rec))) {
    ++counters_.frames_dropped;
  }
  ++counters_.frames_produced;
}

void RecorderApp::OnSegmentEnd(const SegmentEndEvent& ev) {
  if (!hello_done_) return;
  const TxQueueStats qs = client_->queue().Stats();
  const uint64_t dropped =
      qs.audio_dropped >= segment_drop_baseline_ ? qs.audio_dropped - segment_drop_baseline_ : 0;

  SegmentEndEvent copy = ev;
  copy.frames_dropped = static_cast<int>(std::min<uint64_t>(dropped, static_cast<uint64_t>(ev.frames)));
  copy.tx_drops = static_cast<int>(dropped);
  const std::string json = BuildSegmentEnd(copy, capture_id_);
  if (!client_->EnqueueControl(json)) fatal_control_overflow_ = true;
  counters_.frames_dropped = qs.audio_dropped;
  counters_.segments_produced = static_cast<uint64_t>(segmenter_.segments_produced());
  counters_.segments_discarded = static_cast<uint64_t>(segmenter_.segments_discarded());
}

void RecorderApp::ProcessAudio() {
  for (int i = 0; i < 4; ++i) {
    CaptureFrame frame;
    const AvpErr err = source_->Read(frame, 0);
    if (err == AvpErr::kErrTimeout) break;
    if (err != AvpErr::kOk) {
      StopCapture("error", "audio_hw_error");
      return;
    }
    if (frame.data == nullptr || frame.bytes == 0) break;

    InputFrame in;
    in.timestamp_ms = frame.timestamp_ms;
    in.frame_ms = audio_.frame_ms > 0 ? audio_.frame_ms : 20;
    in.discontinuity = frame.discontinuity;

    if (frame.format == AudioFormat::kPcmS16le) {
      in.pcm = frame.pcm;
      in.pcm_samples = frame.pcm_samples;
      in.samples = frame.samples;
      if (audio_.format == AudioFormat::kPcmS16le) {
        in.payload = frame.data;
        in.payload_len = frame.bytes;
        in.format = AudioFormat::kPcmS16le;
      } else {
        encoded_.clear();
        if (encoder_.Encode(frame.pcm, frame.samples, encoded_) != AvpErr::kOk ||
            encoded_.empty()) {
          LOG_W(kTag, "opus encode failed; dropping frame");
          continue;
        }
        in.payload = encoded_.data();
        in.payload_len = encoded_.size();
        in.format = AudioFormat::kOpus;
      }
    } else {
      // Chip-owned Opus: no local PCM, no header stats.
      in.payload = frame.data;
      in.payload_len = frame.bytes;
      in.samples = frame.samples;
      in.format = AudioFormat::kOpus;
      in.pcm = frame.pcm;
      in.pcm_samples = frame.pcm_samples;
      in.stats_valid = frame.stats_valid;
      in.stats = frame.stats;
    }

    if (frame.capture_overrun) ++counters_.capture_overruns;
    segmenter_.Push(in);
  }
}

void RecorderApp::ProcessTimers() {
  const int64_t now = clock_->MonotonicMs();

  if (mqtt_ != nullptr) {
    const bool connected = mqtt_->IsCloudConnected();
    if (connected && !mqtt_connected_) {
      mqtt_connected_ = true;
      mqtt_lost_since_ms_ = 0;
      PublishMqttStatus("");
    } else if (!connected && mqtt_connected_) {
      mqtt_connected_ = false;
      mqtt_lost_since_ms_ = now;
      LOG_W(kTag, "MQTT lost while %s", capture_active_ ? "capturing" : "idle");
    }
    if (mqtt_->TakeConnectedEvent()) PublishMqttStatus("");
  }

  if (capture_active_) {
    if (control_.max_capture_ms > 0 && now - capture_start_ms_ >= control_.max_capture_ms) {
      StopCapture("max_duration", "max_capture");
    } else if (mqtt_ != nullptr && !mqtt_connected_ && mqtt_lost_since_ms_ > 0 &&
               now - mqtt_lost_since_ms_ >= control_.mqtt_lost_stop_timeout_ms) {
      StopCapture("stop_capture", "mqtt_unavailable");
    } else {
      if (telemetry_.level_interval_ms > 0 &&
          now - telemetry_last_ms_ >= telemetry_.level_interval_ms) {
        SendTelemetry();
        telemetry_last_ms_ = now;
      }
      if (telemetry_.stats_interval_ms > 0 &&
          now - stats_last_ms_ >= telemetry_.stats_interval_ms) {
        const std::string stats = BuildStatsJson(MakeStats());
        if (!client_->EnqueueControl(stats)) fatal_control_overflow_ = true;
        stats_last_ms_ = now;
      }
    }
  }

  if (state_ == RecorderState::kDraining && !capture_active_ && now >= drain_deadline_ms_) {
    if (control_.ws_mode == "on_demand" && !recording_intent_) {
      client_->Disconnect();
    }
    SetState(RecorderState::kIdle);
    PublishMqttStatus("");
  }

  if (!client_->IsConnected() && now >= next_reconnect_ms_) {
    if (recording_intent_) {
      if (state_ != RecorderState::kConnectingWs) SetState(RecorderState::kConnectingWs);
      ConnectWs();
    } else if (control_.ws_mode == "always" &&
               (state_ == RecorderState::kIdle || state_ == RecorderState::kError)) {
      ConnectWs();
    }
  }
}

void RecorderApp::SendCaptureState(const std::string& state) {
  if (!hello_done_) return;
  const TxQueueStats qs = client_->queue().Stats();
  const std::string json = BuildCaptureState(
      state, capture_id_, CaptureModeName(capture_.mode), &audio_, &capture_,
      deps_.capabilities.gain_supported, capture_.gain_db, client_->queue().AudioLevelMs(),
      static_cast<int>(qs.audio_dropped), static_cast<int>(client_->send_errors()),
      static_cast<int>(counters_.reconnects));
  if (!client_->EnqueueControl(json)) fatal_control_overflow_ = true;
}

void RecorderApp::SendTelemetry() {
  if (!hello_done_) return;
  TelemetrySnapshot s;
  s.t_ms = clock_->MonotonicMs();
  s.capture_id = capture_id_;
  s.vad_state = capture_active_ ? (segmenter_.in_segment() ? "speech" : "silence") : "unknown";
  s.vad_score = segmenter_.last_vad_score();
  const double rms = segmenter_.last_rms_dbfs();
  s.has_levels = capture_active_ && rms > -190.0;
  s.rms_dbfs = rms;
  s.peak_dbfs = segmenter_.last_peak_dbfs();
  s.noise_dbfs = segmenter_.noise_floor_dbfs();
  s.has_gain = deps_.capabilities.gain_supported;
  s.gain_db = capture_.gain_db;
  const TxQueueStats qs = client_->queue().Stats();
  s.tx_queue_ms = client_->queue().AudioLevelMs();
  s.tx_drops = static_cast<int>(qs.audio_dropped);
  s.tx_errors = static_cast<int>(client_->send_errors());
  s.capture_overruns = static_cast<int>(source_->Stats().overruns);
  s.reconnects = static_cast<int>(counters_.reconnects);
  s.uptime_ms = clock_->MonotonicMs();
  if (!client_->EnqueueControl(BuildTelemetry(s))) fatal_control_overflow_ = true;
}

std::string RecorderApp::PublishMqttStatus(const std::string& id, const std::string& error_code,
                                           const std::string& error_message, bool retryable) {
  if (mqtt_ == nullptr) return std::string();
  MqttStatusData d;
  d.state = state_;
  d.capture_id = capture_id_;
  d.segment_id = segmenter_.current_segment_id();
  d.has_audio = source_configured_ || capture_active_ || hello_done_;
  d.audio = audio_;
  d.gain_supported = deps_.capabilities.gain_supported;
  d.gain_db = capture_.gain_db;
  d.tx_ms = client_->queue().AudioLevelMs();
  const TxQueueStats qs = client_->queue().Stats();
  d.drops = static_cast<int>(qs.audio_dropped);
  d.reconnects = static_cast<int>(counters_.reconnects);
  d.has_error = !error_code.empty();
  d.error_code = error_code;
  d.error_message = error_message.empty() ? error_code : error_message;
  d.error_retryable = retryable;
  d.ts_ms = clock_->WallMs();
  const std::string json = BuildMqttStatus(id, d);
  if (json.size() > 1024) {
    LOG_W(kTag, "recorderStatus is %zu bytes; thing-model length is 1024", json.size());
  }
  mqtt_->PublishStatus(json);
  return json;
}

void RecorderApp::ProcessMqttCommands() {
  if (mqtt_ == nullptr) return;
  std::string text;
  for (int i = 0; i < 16 && mqtt_->TakeCommand(&text); ++i) {
    HandleMqttEnvelope(text);
  }
}

void RecorderApp::HandleMqttEnvelope(const std::string& text) {
  MqttCommand cmd;
  std::string error;
  if (!ParseMqttCommand(text.data(), text.size(), &cmd, &error)) {
    PublishMqttStatus("", ErrorCodeFromMessage(error), error, false);
    return;
  }

  std::string cached;
  if (FindMqttStatus(cmd.id, &cached)) {
    if (mqtt_ != nullptr) mqtt_->PublishStatus(cached);
    return;
  }

  if (cmd.has_expires_at && cmd.expires_at_ms > 0 && clock_->WallMs() > cmd.expires_at_ms) {
    const std::string status = PublishMqttStatus(cmd.id, "expired", "command expired", false);
    RememberMqttStatus(cmd.id, status);
    return;
  }

  if (cmd.action == "start") {
    StartParams sp;
    std::string code;
    std::string message;
    if (!StartParamsFromMqtt(cmd, &sp, &code, &message)) {
      const std::string status = PublishMqttStatus(cmd.id, code, message, false);
      RememberMqttStatus(cmd.id, status);
      return;
    }
    if (capture_active_ && !sp.restart) {
      const std::string status = PublishMqttStatus(cmd.id);
      RememberMqttStatus(cmd.id, status);
      return;
    }

    recording_intent_ = true;
    pending_start_ = sp;
    has_pending_start_ = true;
    pending_mqtt_id_ = cmd.id;

    if (!client_->IsConnected()) {
      SetState(RecorderState::kConnectingWs);
      ConnectWs();
      const std::string status = PublishMqttStatus(cmd.id);
      RememberMqttStatus(cmd.id, status);
      return;
    }
    if (!hello_done_) {
      const std::string status = PublishMqttStatus(cmd.id);
      RememberMqttStatus(cmd.id, status);
      return;
    }

    const std::string status = PublishMqttStatus(cmd.id);
    std::string ec;
    std::string em;
    if (!StartCapture(sp, &ec, &em)) {
      recording_intent_ = false;
      const std::string failed = PublishMqttStatus(cmd.id, ec, em, false);
      RememberMqttStatus(cmd.id, failed);
    } else {
      const std::string now = PublishMqttStatus(cmd.id);
      RememberMqttStatus(cmd.id, now);
    }
    (void)status;
    return;
  }

  if (cmd.action == "stop") {
    recording_intent_ = false;
    if (capture_active_) {
      StopCapture("stop_capture", "", cmd.id);
    } else {
      SetState(RecorderState::kIdle);
      const std::string status = PublishMqttStatus(cmd.id);
      RememberMqttStatus(cmd.id, status);
    }
    return;
  }

  if (cmd.action == "configure") {
    if (capture_active_) {
      const std::string status =
          PublishMqttStatus(cmd.id, "capture_active", "stop capture before configuring", false);
      RememberMqttStatus(cmd.id, status);
      return;
    }
    const rapidjson::Value* p = cmd.Params();
    AudioConfig a = audio_;
    CaptureConfig c = capture_;
    std::string err;
    bool ok = true;
    if (p != nullptr && p->HasMember("audio") && (*p)["audio"].IsObject()) {
      ok = ParseAudioConfigJson((*p)["audio"], &a, &err);
    }
    if (ok && p != nullptr && p->HasMember("capture") && (*p)["capture"].IsObject()) {
      ok = ParseCaptureConfigJson((*p)["capture"], &c, &err);
    }
    if (!ok) {
      const std::string status =
          PublishMqttStatus(cmd.id, ErrorCodeFromMessage(err), err, false);
      RememberMqttStatus(cmd.id, status);
      return;
    }
    std::string code;
    std::string message;
    if (!ValidateAudio(a, &code, &message)) {
      const std::string status = PublishMqttStatus(cmd.id, code, message, false);
      RememberMqttStatus(cmd.id, status);
      return;
    }
    if (c.vad.engine == VadEngine::kDevice && !ListHas(deps_.capabilities.vad, "device")) {
      const std::string status =
          PublishMqttStatus(cmd.id, "unsupported_config", "device VAD unavailable", false);
      RememberMqttStatus(cmd.id, status);
      return;
    }
    audio_ = a;
    capture_ = c;
    if (capture_.pre_roll_ms > deps_.capabilities.device_pre_roll_max_ms) {
      capture_.pre_roll_ms = deps_.capabilities.device_pre_roll_max_ms;
    }
    source_configured_ = false;
    const std::string status = PublishMqttStatus(cmd.id);
    RememberMqttStatus(cmd.id, status);
    return;
  }

  if (cmd.action == "status") {
    const std::string status = PublishMqttStatus(cmd.id);
    RememberMqttStatus(cmd.id, status);
    return;
  }

  if (cmd.action == "test_tone") {
    if (!capture_active_) {
      const std::string status =
          PublishMqttStatus(cmd.id, "capture_not_active", "test tone needs an active capture", false);
      RememberMqttStatus(cmd.id, status);
      return;
    }
    const rapidjson::Value* p = cmd.Params();
    ToneMetadata tone;
    std::string err;
    if (p == nullptr || !ParseToneJson(*p, &tone, &err)) {
      const std::string status =
          PublishMqttStatus(cmd.id, ErrorCodeFromMessage(err), err, false);
      RememberMqttStatus(cmd.id, status);
      return;
    }
    const std::string status = PublishMqttStatus(cmd.id);
    RememberMqttStatus(cmd.id, status);
    StartTone(tone, true, false, false, 0, cmd.id, "");
    return;
  }
}

void RecorderApp::HandleWsCommand(const std::string& text) {
  WsCommand cmd;
  std::string error;
  if (!ParseWsCommand(text.data(), text.size(), &cmd, &error)) {
    client_->SendImmediateControl(BuildErrorJson("bad_message", error, false));
    return;
  }

  std::string cached;
  if (cmd.has_id && FindWsResponse(cmd.id, &cached)) {
    client_->EnqueueControl(cached);
    return;
  }

  auto send_terminal = [&](const std::string& json) {
    client_->EnqueueControl(json);
    if (cmd.has_id) RememberWsResponse(cmd.id, json);
  };
  auto nack = [&](const std::string& code, const std::string& message) {
    send_terminal(BuildNack(cmd.has_id, cmd.id, code, message, false));
  };

  if (cmd.type == "hello_ack") {
    HandleHelloAck(cmd.doc);
    return;
  }

  if (cmd.type == "configure") {
    AudioConfig a = audio_;
    CaptureConfig c = capture_;
    TelemetryConfig t = telemetry_;
    std::string err;
    bool ok = true;
    if (cmd.doc.HasMember("audio") && cmd.doc["audio"].IsObject()) {
      ok = ParseAudioConfigJson(cmd.doc["audio"], &a, &err);
    }
    if (ok && cmd.doc.HasMember("capture") && cmd.doc["capture"].IsObject()) {
      ok = ParseCaptureConfigJson(cmd.doc["capture"], &c, &err);
    }
    if (ok && cmd.doc.HasMember("telemetry") && cmd.doc["telemetry"].IsObject()) {
      ok = ParseTelemetryConfigJson(cmd.doc["telemetry"], &t, &err);
    }
    if (!ok) {
      nack(ErrorCodeFromMessage(err), err);
      return;
    }
    if (capture_active_ && (a != audio_ || c.mode != capture_.mode)) {
      nack("capture_active", "stop capture before changing the audio/capture config");
      return;
    }
    std::string code;
    std::string message;
    if (!ValidateAudio(a, &code, &message)) {
      nack(code, message);
      return;
    }
    audio_ = a;
    capture_ = c;
    telemetry_ = t;
    if (capture_.pre_roll_ms > deps_.capabilities.device_pre_roll_max_ms) {
      capture_.pre_roll_ms = deps_.capabilities.device_pre_roll_max_ms;
    }
    if (a != configured_audio_) source_configured_ = false;
    send_terminal(BuildAck(cmd.has_id, cmd.id));
    return;
  }

  if (cmd.type == "start_capture") {
    const bool ws_can_control =
        control_.owner == "ws" || control_.allow_ws_capture_control;
    if (!ws_can_control) {
      nack("control_owned_by_mqtt", "MQTT owns capture start/stop");
      return;
    }
    StartParams sp;
    std::string code;
    std::string message;
    if (!StartParamsFromWs(cmd, &sp, &code, &message)) {
      nack(code, message);
      return;
    }
    if (!StartCapture(sp, &code, &message)) {
      if (!code.empty()) nack(code, message);
      return;
    }
    send_terminal(BuildAck(cmd.has_id, cmd.id));
    return;
  }

  if (cmd.type == "stop_capture") {
    const bool ws_can_control =
        control_.owner == "ws" || control_.allow_ws_capture_control;
    if (!ws_can_control) {
      nack("control_owned_by_mqtt", "MQTT owns capture start/stop");
      return;
    }
    StopCapture("stop_capture");
    send_terminal(BuildAck(cmd.has_id, cmd.id));
    return;
  }

  if (cmd.type == "set_gain") {
    if (!deps_.capabilities.gain_supported) {
      nack("unsupported_feature", "hardware gain is not supported on this platform");
      return;
    }
    if (capture_active_) {
      nack("capture_active", "gain changes require idle capture");
      return;
    }
    double gain = 0;
    if (!JsonGetDouble(cmd.doc, "gain_db", &gain)) {
      nack("bad_message", "gain_db is required");
      return;
    }
    capture_.gain_db = static_cast<int>(std::lround(gain));
    send_terminal(BuildAck(cmd.has_id, cmd.id));
    return;
  }

  if (cmd.type == "set_rate") {
    if (capture_active_) {
      nack("capture_active", "rate changes require idle capture");
      return;
    }
    int rate = 0;
    if (!JsonGetInt(cmd.doc, "sample_rate", &rate)) {
      nack("bad_message", "sample_rate is required");
      return;
    }
    AudioConfig a = audio_;
    a.sample_rate = rate;
    std::string code;
    std::string message;
    if (!ValidateAudio(a, &code, &message)) {
      nack(code, message);
      return;
    }
    audio_ = a;
    source_configured_ = false;
    send_terminal(BuildAck(cmd.has_id, cmd.id));
    return;
  }

  if (cmd.type == "set_telemetry") {
    std::string err;
    if (!ParseTelemetryConfigJson(cmd.doc, &telemetry_, &err)) {
      nack(ErrorCodeFromMessage(err), err);
      return;
    }
    send_terminal(BuildAck(cmd.has_id, cmd.id));
    return;
  }

  if (cmd.type == "get_stats") {
    send_terminal(BuildAck(cmd.has_id, cmd.id, BuildStatsJson(MakeStats())));
    return;
  }

  if (cmd.type == "get_status") {
    send_terminal(BuildAck(cmd.has_id, cmd.id, BuildStatusJson(MakeStats())));
    return;
  }

  if (cmd.type == "reset_stats") {
    ResetStats();
    send_terminal(BuildAck(cmd.has_id, cmd.id));
    return;
  }

  if (cmd.type == "test_tone") {
    HandleWsTestTone(cmd);
    return;
  }

  if (cmd.type == "ping") {
    const int64_t t0 = clock_->MonotonicMs();
    client_->SendImmediateControl(BuildPong(cmd.has_id, cmd.id, t0, t0, false, 0));
    return;
  }

  if (cmd.type == "play_audio") {
    nack("unsupported_feature", "play_audio is reserved in v1");
    return;
  }

  nack("unsupported_feature", "unknown message type: " + cmd.type);
}

void RecorderApp::HandleHelloAck(const rapidjson::Document& doc) {
  auto fail = [&](const std::string& code, const std::string& message) {
    client_->SendImmediateControl(BuildErrorJson(code, message, true));
    client_->Disconnect();
    SetState(RecorderState::kError);
    last_error_code_ = code;
    recording_intent_ = false;
    PublishMqttStatus("", code, message, false);
    LOG_E(kTag, "handshake rejected: %s (%s)", code.c_str(), message.c_str());
  };

  int version = 0;
  if (!JsonGetInt(doc, "protocol_version", &version) || version != protocol::kProtocolVersion) {
    fail("unsupported_version", "protocol_version mismatch");
    return;
  }

  if (doc.HasMember("control") && doc["control"].IsObject()) {
    const rapidjson::Value& c = doc["control"];
    std::string owner;
    if (JsonGetString(c, "owner", &owner)) {
      if (owner != "mqtt" && owner != "ws") {
        fail("unsupported_config", "invalid control.owner");
        return;
      }
      control_.owner = owner;
    }
    std::string ws_mode;
    if (JsonGetString(c, "ws_mode", &ws_mode)) {
      if (ws_mode != "on_demand" && ws_mode != "always") {
        fail("unsupported_config", "invalid control.ws_mode");
        return;
      }
      control_.ws_mode = ws_mode;
    }
    if (c.HasMember("allow_ws_capture_control")) {
      JsonGetBool(c, "allow_ws_capture_control", &control_.allow_ws_capture_control);
    }
  }

  std::string capture_id;
  if (!JsonGetString(doc, "capture_id", &capture_id) || capture_id.empty()) {
    fail("bad_message", "hello_ack.capture_id is required");
    return;
  }
  capture_id_ = capture_id;
  std::string session_id;
  JsonGetString(doc, "session_id", &session_id);

  if (doc.HasMember("limits") && doc["limits"].IsObject()) {
    int server_max = 0;
    if (JsonGetInt(doc["limits"], "max_frame_bytes", &server_max) && server_max > 0) {
      limits_.max_frame_bytes = std::min(limits_.max_frame_bytes, server_max);
    }
    int server_queue = 0;
    if (JsonGetInt(doc["limits"], "max_queue_ms", &server_queue) && server_queue > 0) {
      limits_.max_queue_ms = std::min(limits_.max_queue_ms, server_queue);
    }
  }

  if (!doc.HasMember("audio") || !doc["audio"].IsObject()) {
    fail("unsupported_config", "hello_ack.audio is required");
    return;
  }
  AudioConfig a = audio_;
  std::string err;
  if (!ParseAudioConfigJson(doc["audio"], &a, &err)) {
    fail(ErrorCodeFromMessage(err), err);
    return;
  }
  if (a.format == AudioFormat::kOpus && a.opus_encoder.empty()) {
    a.opus_encoder = source_->NativeFormat() == AudioFormat::kPcmS16le ? "host" : "gx8006";
  }
  std::string code;
  std::string message;
  if (!ValidateAudio(a, &code, &message)) {
    fail(code, message);
    return;
  }

  CaptureConfig c = capture_;
  if (doc.HasMember("capture") && doc["capture"].IsObject()) {
    if (!ParseCaptureConfigJson(doc["capture"], &c, &err)) {
      fail(ErrorCodeFromMessage(err), err);
      return;
    }
  }
  if (c.pre_roll_ms > deps_.capabilities.device_pre_roll_max_ms) {
    c.pre_roll_ms = deps_.capabilities.device_pre_roll_max_ms;
  }
  if (c.vad.engine == VadEngine::kDevice && !ListHas(deps_.capabilities.vad, "device")) {
    fail("unsupported_config", "device VAD is not available on this platform");
    return;
  }

  TelemetryConfig t = telemetry_;
  if (doc.HasMember("telemetry") && doc["telemetry"].IsObject()) {
    if (!ParseTelemetryConfigJson(doc["telemetry"], &t, &err)) {
      fail(ErrorCodeFromMessage(err), err);
      return;
    }
  }

  bool server_start = false;
  if (doc.HasMember("start") && doc["start"].IsBool()) server_start = doc["start"].GetBool();

  audio_ = a;
  capture_ = c;
  telemetry_ = t;
  hello_done_ = true;
  LOG_I(kTag, "hello_ack: %s %d Hz %d ch %d ms, mode=%s", AudioFormatName(audio_.format),
        audio_.sample_rate, audio_.channels, audio_.frame_ms, CaptureModeName(capture_.mode));

  StartParams sp;
  if (recording_intent_) {
    if (has_pending_start_) sp = pending_start_;
    sp.source = CommandSource::kMqtt;
  } else if (control_.owner == "ws" && server_start) {
    sp.source = CommandSource::kWs;
  }

  if (recording_intent_) {
    std::string ec;
    std::string em;
    if (!StartCapture(sp, &ec, &em)) {
      fail(ec.empty() ? "internal" : ec, em);
      return;
    }
  } else if (control_.owner == "ws" && server_start) {
    std::string ec;
    std::string em;
    if (!StartCapture(sp, &ec, &em)) {
      fail(ec.empty() ? "internal" : ec, em);
      return;
    }
  } else {
    SendCaptureState("idle");
  }
}

void RecorderApp::HandleWsTestTone(const WsCommand& cmd) {
  auto nack = [&](const std::string& code, const std::string& message) {
    const std::string json = BuildNack(cmd.has_id, cmd.id, code, message, false);
    client_->EnqueueControl(json);
    if (cmd.has_id) RememberWsResponse(cmd.id, json);
  };

  if (!deps_.capabilities.test_tone || deps_.sink == nullptr || !deps_.sink->ready()) {
    nack("unsupported_feature", "test tone is not available on this platform");
    return;
  }
  if (tone_active_) {
    nack("busy", "another test tone is active");
    return;
  }

  ToneMetadata tone;
  std::string err;
  if (!ParseToneJson(cmd.doc, &tone, &err)) {
    nack(ErrorCodeFromMessage(err), err);
    return;
  }
  if (tone.loopback && !deps_.capabilities.loopback) {
    nack("unsupported_feature", "loopback is not available");
    return;
  }
  bool capture = false;
  JsonGetBool(cmd.doc, "capture", &capture);
  if (capture && !capture_active_) {
    nack("capture_not_active", "test tone capture needs an active capture");
    return;
  }
  std::string label;
  JsonGetString(cmd.doc, "label", &label);

  client_->EnqueueControl(BuildAccepted(cmd.has_id, cmd.id, "tone scheduled"));
  StartTone(tone, capture, true, cmd.has_id, cmd.id, "", label);
}

void RecorderApp::StartTone(const ToneMetadata& tone, bool capture, bool ws_command,
                            bool has_id, int64_t id, const std::string& mqtt_id,
                            const std::string& label) {
  auto fail = [&](const std::string& code, const std::string& detail) {
    LOG_E(kTag, "test tone failed: %s (%s)", code.c_str(), detail.c_str());
    if (ws_command) {
      const std::string json = BuildFailed(has_id, id, code, detail);
      client_->EnqueueControl(json);
      if (has_id) RememberWsResponse(id, json);
    } else if (!mqtt_id.empty()) {
      const std::string status = PublishMqttStatus(mqtt_id, code, detail, false);
      RememberMqttStatus(mqtt_id, status);
    }
  };

  if (deps_.sink == nullptr || deps_.sink->Init(kToneSampleRate, kToneChannels) != AvpErr::kOk) {
    fail("audio_hw_error", "speaker init failed");
    return;
  }
  deps_.sink->SetVolume(tone.speaker_volume);
  if (tone_gen_.Init(tone, kToneSampleRate, kToneChannels) != AvpErr::kOk) {
    deps_.sink->Deinit();
    fail("unsupported_config", "invalid tone parameters");
    return;
  }

  tone_active_ = true;
  tone_start_sent_ = false;
  tone_ws_command_ = ws_command;
  tone_has_id_ = has_id;
  tone_command_id_ = id;
  tone_mqtt_id_ = mqtt_id;
  tone_capture_ = capture;
  tone_meta_ = tone;
  tone_start_ms_ = clock_->MonotonicMs();
  tone_buf_frames_ = 0;
  tone_buf_pos_ = 0;

  if (capture) {
    // The segment opens on the next captured frame, which also emits
    // tone_start (from OnSegmentStart) before segment_start.
    segmenter_.BeginManual(label, tone);
  } else {
    const std::string start =
        BuildToneStart(tone.tone_id, static_cast<uint32_t>(clock_->MonotonicMs()), 0);
    if (!client_->EnqueueControl(start)) fatal_control_overflow_ = true;
    tone_start_sent_ = true;
  }
}

void RecorderApp::ProcessTone() {
  if (!tone_active_) return;

  if (tone_buf_pos_ >= tone_buf_frames_) {
    if (tone_gen_.done()) {
      FinishTone(true);
      return;
    }
    const std::size_t n = tone_gen_.Render(tone_buf_.data(), kToneChunkFrames);
    tone_buf_frames_ = n;
    tone_buf_pos_ = 0;
    if (n == 0) {
      FinishTone(true);
      return;
    }
  }

  const std::size_t frames_left = tone_buf_frames_ - tone_buf_pos_;
  const AvpErr err = deps_.sink->Write(
      tone_buf_.data() + tone_buf_pos_ * kToneChannels, frames_left, 0);
  if (err == AvpErr::kErrBusy || err == AvpErr::kErrTimeout) return;
  if (err != AvpErr::kOk) {
    FinishTone(false, "audio_hw_error");
    return;
  }
  tone_buf_pos_ += frames_left;
  if (tone_gen_.done() && tone_buf_pos_ >= tone_buf_frames_) {
    FinishTone(true);
  }
}

void RecorderApp::FinishTone(bool success, const std::string& error_code) {
  if (!tone_active_) return;

  if (tone_capture_) segmenter_.EndManual("max_duration");

  if (!tone_start_sent_) {
    const uint32_t seg = tone_capture_ ? segmenter_.current_segment_id() : 0;
    const std::string start =
        BuildToneStart(tone_meta_.tone_id, static_cast<uint32_t>(clock_->MonotonicMs()), seg);
    if (!client_->EnqueueControl(start)) fatal_control_overflow_ = true;
    tone_start_sent_ = true;
  }

  const uint32_t seg = tone_capture_ ? segmenter_.current_segment_id() : 0;
  const std::string end = BuildToneEnd(tone_meta_.tone_id,
                                       static_cast<uint32_t>(clock_->MonotonicMs()), seg,
                                       success ? "completed" : "failed");
  if (!client_->EnqueueControl(end)) fatal_control_overflow_ = true;

  if (tone_ws_command_) {
    std::string terminal;
    if (success) {
      std::string result = "{\"tone_id\":\"";
      result += tone_meta_.tone_id;
      result += "\",\"segment_id\":";
      result += std::to_string(seg);
      result += "}";
      terminal = BuildCompleted(tone_has_id_, tone_command_id_, result);
    } else {
      terminal = BuildFailed(tone_has_id_, tone_command_id_,
                             error_code.empty() ? "audio_hw_error" : error_code,
                             "test tone failed");
    }
    client_->EnqueueControl(terminal);
    if (tone_has_id_) RememberWsResponse(tone_command_id_, terminal);
  } else if (!tone_mqtt_id_.empty()) {
    const std::string status =
        success ? PublishMqttStatus(tone_mqtt_id_)
                : PublishMqttStatus(tone_mqtt_id_,
                                    error_code.empty() ? "audio_hw_error" : error_code,
                                    "test tone failed", false);
    RememberMqttStatus(tone_mqtt_id_, status);
  }

  deps_.sink->Deinit();
  tone_active_ = false;
  tone_start_sent_ = false;
  tone_mqtt_id_.clear();
  LOG_I(kTag, "test tone %s", success ? "completed" : "failed");
}

StatsSnapshot RecorderApp::MakeStats() const {
  StatsSnapshot s;
  s.counters = counters_;
  s.counters.frames_sent = client_->frames_sent();
  s.counters.bytes_sent = client_->bytes_sent();
  s.counters.send_errors = client_->send_errors();
  const TxQueueStats qs = client_->queue().Stats();
  s.counters.frames_dropped = qs.audio_dropped;
  s.counters.tx_queue_high_water = qs.high_water_records;
  s.counters.capture_overruns = source_->Stats().overruns;
  s.counters.segments_produced = static_cast<uint64_t>(segmenter_.segments_produced());
  s.counters.segments_discarded = static_cast<uint64_t>(segmenter_.segments_discarded());
  s.tx_queue_ms = client_->queue().AudioLevelMs();
  s.tx_queue_records = client_->queue().Level();
  s.has_audio = source_configured_;
  s.audio = audio_;
  s.capture_id = capture_id_;
  s.state = RecorderStateName(state_);
  return s;
}

void RecorderApp::ResetStats() {
  counters_ = RecorderStats{};
  client_->ResetCounters();
}

void RecorderApp::RememberWsResponse(int64_t id, const std::string& json) {
  if (id <= 0) return;
  if (ws_dedup_.find(id) == ws_dedup_.end()) ws_dedup_order_.push_back(id);
  ws_dedup_[id] = json;
  while (ws_dedup_order_.size() > kMaxDedup) {
    ws_dedup_.erase(ws_dedup_order_.front());
    ws_dedup_order_.pop_front();
  }
}

bool RecorderApp::FindWsResponse(int64_t id, std::string* json) const {
  auto it = ws_dedup_.find(id);
  if (it == ws_dedup_.end()) return false;
  *json = it->second;
  return true;
}

void RecorderApp::RememberMqttStatus(const std::string& id, const std::string& json) {
  if (id.empty()) return;
  if (mqtt_dedup_.find(id) == mqtt_dedup_.end()) mqtt_dedup_order_.push_back(id);
  mqtt_dedup_[id] = json;
  while (mqtt_dedup_order_.size() > kMaxDedup) {
    mqtt_dedup_.erase(mqtt_dedup_order_.front());
    mqtt_dedup_order_.pop_front();
  }
}

bool RecorderApp::FindMqttStatus(const std::string& id, std::string* json) const {
  auto it = mqtt_dedup_.find(id);
  if (it == mqtt_dedup_.end()) return false;
  *json = it->second;
  return true;
}

}  // namespace recorder
