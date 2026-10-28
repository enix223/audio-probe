#include "cl_recorder_messages.h"

#include <cmath>

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace recorder {

namespace {

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

void WriteString(Writer& w, const std::string& s) { w.String(s.data(), s.size()); }

double Round2(double v) {
  if (!std::isfinite(v)) return -200.0;
  return std::round(v * 100.0) / 100.0;
}

void WriteFormatCaps(Writer& w, const FormatCapability& f, bool opus) {
  w.StartObject();
  w.Key("sample_rates");
  w.StartArray();
  for (int v : f.sample_rates) w.Int(v);
  w.EndArray();
  w.Key("channels");
  w.StartArray();
  for (int v : f.channels) w.Int(v);
  w.EndArray();
  w.Key("frame_ms");
  w.StartArray();
  for (int v : f.frame_ms) w.Int(v);
  w.EndArray();
  if (opus && !f.bitrate.empty()) {
    w.Key("bitrate");
    w.StartArray();
    for (int v : f.bitrate) w.Int(v);
    w.EndArray();
  }
  w.Key("payload_max");
  w.Int(f.payload_max);
  if (opus && !f.encoders.empty()) {
    w.Key("encoders");
    w.StartArray();
    for (const auto& e : f.encoders) WriteString(w, e);
    w.EndArray();
  }
  if (!opus && !f.channel_map.empty()) {
    w.Key("channel_map");
    w.StartArray();
    for (const auto& c : f.channel_map) WriteString(w, c);
    w.EndArray();
  }
  w.EndObject();
}

void WriteAudioConfig(Writer& w, const AudioConfig& a) {
  w.StartObject();
  w.Key("format");
  WriteString(w, AudioFormatName(a.format));
  w.Key("sample_rate");
  w.Int(a.sample_rate);
  w.Key("channels");
  w.Int(a.channels);
  w.Key("frame_ms");
  w.Int(a.frame_ms);
  if (a.format == AudioFormat::kPcmS16le) {
    w.Key("bits_per_sample");
    w.Int(16);
    w.Key("endianness");
    w.String("little");
    w.Key("interleaved");
    w.Bool(true);
  } else {
    w.Key("opus");
    w.StartObject();
    if (!a.opus_encoder.empty()) {
      w.Key("encoder");
      WriteString(w, a.opus_encoder);
    }
    if (a.opus_bitrate > 0) {
      w.Key("bitrate");
      w.Int(a.opus_bitrate);
    }
    if (a.opus_complexity >= 0) {
      w.Key("complexity");
      w.Int(a.opus_complexity);
    }
    if (!a.opus_application.empty()) {
      w.Key("application");
      WriteString(w, a.opus_application);
    }
    w.EndObject();
  }
  w.EndObject();
}

void WriteTone(Writer& w, const ToneMetadata& t) {
  w.StartObject();
  w.Key("tone_id");
  WriteString(w, t.tone_id);
  w.Key("waveform");
  WriteString(w, t.waveform);
  if (t.waveform == "sweep") {
    w.Key("start_hz");
    w.Double(t.start_hz);
    w.Key("end_hz");
    w.Double(t.end_hz);
    w.Key("sweep_mode");
    WriteString(w, t.sweep_mode.empty() ? "linear" : t.sweep_mode);
  } else {
    w.Key("frequency_hz");
    w.Double(t.frequency_hz);
  }
  w.Key("amplitude_dbfs");
  w.Double(t.amplitude_dbfs);
  w.Key("speaker_volume");
  w.Int(t.speaker_volume);
  w.Key("capture_path");
  WriteString(w, t.capture_path);
  w.Key("loopback");
  w.Bool(t.loopback);
  w.Key("fade_ms");
  w.Int(t.fade_ms);
  w.EndObject();
}

void WriteRawObjectOrEmpty(Writer& w, const std::string& json) {
  if (json.empty()) {
    w.StartObject();
    w.EndObject();
    return;
  }
  w.RawValue(json.c_str(), json.size(), rapidjson::kObjectType);
}

}  // namespace

std::string BuildHello(const HelloContext& ctx) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("hello");
  w.Key("protocol_version");
  w.Int(protocol::kProtocolVersion);
  w.Key("device");
  w.StartObject();
  w.Key("id");
  WriteString(w, ctx.device.id);
  w.Key("model");
  WriteString(w, ctx.device.model);
  w.Key("firmware");
  WriteString(w, ctx.device.firmware);
  w.Key("chip");
  WriteString(w, ctx.device.chip);
  w.Key("audio_frontend");
  WriteString(w, ctx.device.audio_frontend);
  w.Key("boot_id");
  WriteString(w, ctx.device.boot_id);
  w.EndObject();
  w.Key("clock");
  w.StartObject();
  w.Key("monotonic_ms");
  w.Int64(ctx.clock.monotonic_ms);
  w.Key("wall_ms");
  w.Int64(ctx.clock.wall_ms);
  w.Key("synced");
  w.Bool(ctx.clock.synced);
  w.Key("capture_t0_mono_us");
  w.Int64(ctx.clock.capture_t0_mono_us);
  w.EndObject();
  w.Key("control");
  w.StartObject();
  w.Key("owner");
  WriteString(w, ctx.control.owner);
  w.Key("allow_ws_capture_control");
  w.Bool(ctx.control.allow_ws_capture_control);
  w.Key("ws_mode");
  WriteString(w, ctx.control.ws_mode);
  w.Key("mqtt_lost_stop_timeout_ms");
  w.Int(ctx.control.mqtt_lost_stop_timeout_ms);
  w.Key("max_capture_ms");
  w.Int(ctx.control.max_capture_ms);
  w.EndObject();

  w.Key("capabilities");
  w.StartObject();
  w.Key("formats");
  w.StartObject();
  if (ctx.capabilities.pcm_s16le) {
    w.Key("pcm_s16le");
    WriteFormatCaps(w, ctx.capabilities.pcm, false);
  }
  if (ctx.capabilities.opus) {
    w.Key("opus");
    WriteFormatCaps(w, ctx.capabilities.opus_cap, true);
  }
  w.EndObject();
  w.Key("vad");
  w.StartArray();
  for (const auto& v : ctx.capabilities.vad) WriteString(w, v);
  w.EndArray();
  if (ctx.capabilities.gain_supported) {
    w.Key("gain");
    w.StartObject();
    w.Key("min");
    w.Int(ctx.capabilities.gain_min);
    w.Key("max");
    w.Int(ctx.capabilities.gain_max);
    w.Key("step");
    w.Int(ctx.capabilities.gain_step);
    w.Key("unit");
    WriteString(w, ctx.capabilities.gain_unit);
    w.EndObject();
  }
  if (ctx.capabilities.test_tone) {
    w.Key("test_tone");
    w.StartObject();
    w.Key("waveforms");
    w.StartArray();
    for (const auto& t : ctx.capabilities.tone_waveforms) WriteString(w, t);
    w.EndArray();
    w.Key("frequency_hz");
    w.StartObject();
    w.Key("min");
    w.Double(ctx.capabilities.tone_min_hz);
    w.Key("max");
    w.Double(ctx.capabilities.tone_max_hz);
    w.EndObject();
    w.Key("max_amplitude_dbfs");
    w.Double(ctx.capabilities.tone_max_amplitude_dbfs);
    w.EndObject();
  }
  if (ctx.capabilities.speaker) {
    w.Key("speaker");
    w.StartObject();
    w.Key("min");
    w.Int(ctx.capabilities.speaker_min);
    w.Key("max");
    w.Int(ctx.capabilities.speaker_max);
    w.Key("unit");
    w.String("percent");
    w.EndObject();
  }
  w.Key("loopback");
  w.Bool(ctx.capabilities.loopback);
  w.Key("device_pre_roll_max_ms");
  w.Int(ctx.capabilities.device_pre_roll_max_ms);
  w.Key("server_pre_roll");
  w.Bool(ctx.capabilities.server_pre_roll);
  w.Key("max_segment_ms");
  w.Int(ctx.capabilities.max_segment_ms);
  w.Key("tx_queue_ms");
  w.Int(ctx.capabilities.tx_queue_ms);
  w.Key("limits");
  w.StartObject();
  w.Key("max_frame_bytes");
  w.Int(ctx.capabilities.max_frame_bytes);
  w.EndObject();
  w.EndObject();  // capabilities

  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildCaptureState(const std::string& state, const std::string& capture_id,
                              const std::string& mode, const AudioConfig* audio,
                              const CaptureConfig* capture, bool gain_supported, int gain_db,
                              int tx_queue_ms, int tx_drops, int tx_errors, int reconnects) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("capture_state");
  w.Key("state");
  WriteString(w, state);
  if (!capture_id.empty()) {
    w.Key("capture_id");
    WriteString(w, capture_id);
  }
  if (!mode.empty()) {
    w.Key("mode");
    WriteString(w, mode);
  }
  if (audio != nullptr) {
    w.Key("audio");
    WriteAudioConfig(w, *audio);
  }
  if (capture != nullptr) {
    w.Key("capture");
    w.StartObject();
    w.Key("pre_roll_ms");
    w.Int(capture->pre_roll_ms);
    w.Key("vad_engine");
    WriteString(w, VadEngineName(capture->vad.engine));
    w.Key("max_segment_ms");
    w.Int(capture->max_segment_ms);
    w.EndObject();
  }
  w.Key("gain_db");
  if (gain_supported) {
    w.Double(gain_db);
  } else {
    w.Null();
  }
  w.Key("gain_supported");
  w.Bool(gain_supported);
  w.Key("tx");
  w.StartObject();
  w.Key("queue_ms");
  w.Int(tx_queue_ms);
  w.Key("drops");
  w.Int(tx_drops);
  w.Key("errors");
  w.Int(tx_errors);
  w.Key("reconnects");
  w.Int(reconnects);
  w.EndObject();
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildSegmentStart(const SegmentStartEvent& ev, const std::string& capture_id,
                              const AudioConfig& audio, bool gain_supported, int gain_db) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("segment_start");
  w.Key("capture_id");
  WriteString(w, capture_id);
  w.Key("segment_id");
  w.Uint(ev.segment_id);
  w.Key("reason");
  WriteString(w, ev.reason);
  w.Key("t_ms");
  w.Uint(ev.t_ms);
  w.Key("onset_t_ms");
  w.Uint(ev.onset_t_ms);
  w.Key("pre_roll_ms");
  w.Int(ev.pre_roll_ms);
  w.Key("audio");
  WriteAudioConfig(w, audio);
  w.Key("gain_db");
  if (gain_supported) {
    w.Double(gain_db);
  } else {
    w.Null();
  }
  if (!ev.label.empty()) {
    w.Key("label");
    WriteString(w, ev.label);
  }
  w.Key("tone");
  if (ev.has_tone) {
    WriteTone(w, ev.tone);
  } else {
    w.Null();
  }
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildSegmentEnd(const SegmentEndEvent& ev, const std::string& capture_id) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("segment_end");
  w.Key("capture_id");
  WriteString(w, capture_id);
  w.Key("segment_id");
  w.Uint(ev.segment_id);
  w.Key("reason");
  WriteString(w, ev.reason);
  w.Key("t_ms");
  w.Uint(ev.t_ms);
  w.Key("frames");
  w.Int(ev.frames);
  w.Key("frames_dropped");
  w.Int(ev.frames_dropped);
  w.Key("pre_roll_frames");
  w.Int(ev.pre_roll_frames);
  w.Key("duration_ms");
  w.Int(ev.duration_ms);
  w.Key("speech_ms");
  w.Int(ev.speech_ms);
  w.Key("rms_dbfs");
  w.Double(Round2(ev.rms_dbfs));
  w.Key("peak_dbfs");
  w.Double(Round2(ev.peak_dbfs));
  w.Key("clip_pct");
  w.Double(Round2(ev.clip_pct));
  w.Key("noise_dbfs");
  w.Double(Round2(ev.noise_dbfs));
  w.Key("tx_drops");
  w.Int(ev.tx_drops);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildTelemetry(const TelemetrySnapshot& s) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("telemetry");
  w.Key("t_ms");
  w.Int64(s.t_ms);
  if (!s.capture_id.empty()) {
    w.Key("capture_id");
    WriteString(w, s.capture_id);
  }
  w.Key("vad_state");
  WriteString(w, s.vad_state);
  w.Key("vad_score");
  w.Int(s.vad_score);
  w.Key("rms_dbfs");
  if (s.has_levels) {
    w.Double(Round2(s.rms_dbfs));
  } else {
    w.Null();
  }
  w.Key("peak_dbfs");
  if (s.has_levels) {
    w.Double(Round2(s.peak_dbfs));
  } else {
    w.Null();
  }
  w.Key("noise_dbfs");
  if (s.has_levels) {
    w.Double(Round2(s.noise_dbfs));
  } else {
    w.Null();
  }
  w.Key("gain_db");
  if (s.has_gain) {
    w.Double(s.gain_db);
  } else {
    w.Null();
  }
  w.Key("tx_queue_ms");
  w.Int(s.tx_queue_ms);
  w.Key("tx_drops");
  w.Int(s.tx_drops);
  w.Key("tx_errors");
  w.Int(s.tx_errors);
  w.Key("capture_overruns");
  w.Int(s.capture_overruns);
  w.Key("reconnects");
  w.Int(s.reconnects);
  if (s.has_rssi) {
    w.Key("rssi_dbm");
    w.Int(s.rssi_dbm);
  }
  if (s.has_heap) {
    w.Key("heap_free");
    w.Int64(s.heap_free);
  }
  w.Key("uptime_ms");
  w.Int64(s.uptime_ms);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildAck(bool has_id, int64_t id, const std::string& result_json) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("ack");
  if (has_id) {
    w.Key("id");
    w.Int64(id);
  }
  w.Key("result");
  WriteRawObjectOrEmpty(w, result_json);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildNack(bool has_id, int64_t id, const std::string& code,
                      const std::string& message, bool retryable) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("nack");
  if (has_id) {
    w.Key("id");
    w.Int64(id);
  }
  w.Key("code");
  WriteString(w, code);
  w.Key("message");
  WriteString(w, message);
  w.Key("retryable");
  w.Bool(retryable);
  w.Key("detail");
  w.StartObject();
  w.EndObject();
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildAccepted(bool has_id, int64_t id, const std::string& detail) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("accepted");
  if (has_id) {
    w.Key("id");
    w.Int64(id);
  }
  w.Key("detail");
  WriteString(w, detail);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildCompleted(bool has_id, int64_t id, const std::string& result_json) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("completed");
  if (has_id) {
    w.Key("id");
    w.Int64(id);
  }
  w.Key("result");
  WriteRawObjectOrEmpty(w, result_json);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildFailed(bool has_id, int64_t id, const std::string& code,
                        const std::string& detail) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("failed");
  if (has_id) {
    w.Key("id");
    w.Int64(id);
  }
  w.Key("code");
  WriteString(w, code);
  w.Key("detail");
  WriteString(w, detail);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildErrorJson(const std::string& code, const std::string& message, bool fatal) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("error");
  w.Key("code");
  WriteString(w, code);
  w.Key("message");
  WriteString(w, message);
  w.Key("fatal");
  w.Bool(fatal);
  w.Key("detail");
  w.StartObject();
  w.EndObject();
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildPong(bool has_id, int64_t id, int64_t t0_ms, int64_t t1_ms,
                      bool has_server_time, int64_t server_time_ms) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("pong");
  if (has_id) {
    w.Key("id");
    w.Int64(id);
  }
  w.Key("t0_ms");
  w.Int64(t0_ms);
  w.Key("t1_ms");
  w.Int64(t1_ms);
  if (has_server_time) {
    w.Key("server_time_ms");
    w.Int64(server_time_ms);
  }
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildToneStart(const std::string& tone_id, uint32_t t_ms, uint32_t segment_id) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("tone_start");
  w.Key("tone_id");
  WriteString(w, tone_id);
  w.Key("t_ms");
  w.Uint(t_ms);
  w.Key("segment_id");
  w.Uint(segment_id);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildToneEnd(const std::string& tone_id, uint32_t t_ms, uint32_t segment_id,
                         const std::string& reason) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("tone_end");
  w.Key("tone_id");
  WriteString(w, tone_id);
  w.Key("t_ms");
  w.Uint(t_ms);
  w.Key("segment_id");
  w.Uint(segment_id);
  w.Key("reason");
  WriteString(w, reason);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildMqttStatus(const std::string& id, const MqttStatusData& d) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("v");
  w.Int(1);
  w.Key("type");
  w.String("recorder_status");
  w.Key("id");
  WriteString(w, id);
  w.Key("state");
  w.String(RecorderStateName(d.state));
  if (!d.capture_id.empty()) {
    w.Key("capture_id");
    WriteString(w, d.capture_id);
  }
  if (d.segment_id != 0) {
    w.Key("segment_id");
    w.Uint(d.segment_id);
  }
  if (d.has_audio) {
    w.Key("audio");
    WriteAudioConfig(w, d.audio);
  }
  w.Key("gain_db");
  if (d.gain_supported) {
    w.Double(d.gain_db);
  } else {
    w.Null();
  }
  w.Key("queue");
  w.StartObject();
  w.Key("tx_ms");
  w.Int(d.tx_ms);
  w.Key("drops");
  w.Int(d.drops);
  w.Key("reconnects");
  w.Int(d.reconnects);
  w.EndObject();
  w.Key("error");
  if (d.has_error) {
    w.StartObject();
    w.Key("code");
    WriteString(w, d.error_code);
    w.Key("message");
    WriteString(w, d.error_message);
    w.Key("retryable");
    w.Bool(d.error_retryable);
    w.EndObject();
  } else {
    w.Null();
  }
  w.Key("ts_ms");
  w.Int64(d.ts_ms);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildStatsJson(const StatsSnapshot& s) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("stats");
  w.Key("state");
  WriteString(w, s.state);
  if (!s.capture_id.empty()) {
    w.Key("capture_id");
    WriteString(w, s.capture_id);
  }
  if (s.has_audio) {
    w.Key("audio");
    WriteAudioConfig(w, s.audio);
  }
  w.Key("tx_queue_ms");
  w.Int(s.tx_queue_ms);
  w.Key("tx_queue_records");
  w.Uint64(s.tx_queue_records);
  w.Key("frames_produced");
  w.Uint64(s.counters.frames_produced);
  w.Key("frames_sent");
  w.Uint64(s.counters.frames_sent);
  w.Key("frames_dropped");
  w.Uint64(s.counters.frames_dropped);
  w.Key("bytes_sent");
  w.Uint64(s.counters.bytes_sent);
  w.Key("send_errors");
  w.Uint64(s.counters.send_errors);
  w.Key("capture_overruns");
  w.Uint64(s.counters.capture_overruns);
  w.Key("reconnects");
  w.Uint64(s.counters.reconnects);
  w.Key("segments_produced");
  w.Uint64(s.counters.segments_produced);
  w.Key("segments_discarded");
  w.Uint64(s.counters.segments_discarded);
  w.Key("tx_queue_high_water");
  w.Uint64(s.counters.tx_queue_high_water);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

std::string BuildStatusJson(const StatsSnapshot& s) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");
  w.String("status");
  w.Key("state");
  WriteString(w, s.state);
  if (!s.capture_id.empty()) {
    w.Key("capture_id");
    WriteString(w, s.capture_id);
  }
  if (s.has_audio) {
    w.Key("audio");
    WriteAudioConfig(w, s.audio);
  }
  w.Key("tx_queue_ms");
  w.Int(s.tx_queue_ms);
  w.EndObject();
  return std::string(buf.GetString(), buf.GetSize());
}

}  // namespace recorder
