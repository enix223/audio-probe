#include "cl_recorder_commands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "rapidjson/error/en.h"

namespace recorder {

namespace {

std::string ParseErrorString(const rapidjson::Document& doc) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "bad_message: JSON error at offset %zu: %s",
                static_cast<std::size_t>(doc.GetErrorOffset()),
                rapidjson::GetParseError_En(doc.GetParseError()));
  return buf;
}

bool ReadStringOrNumber(const rapidjson::Value& v, std::string* out) {
  if (v.IsString()) {
    *out = std::string(v.GetString(), v.GetStringLength());
    return true;
  }
  if (v.IsUint64()) {
    *out = std::to_string(v.GetUint64());
    return true;
  }
  if (v.IsInt64()) {
    *out = std::to_string(v.GetInt64());
    return true;
  }
  if (v.IsInt()) {
    *out = std::to_string(v.GetInt());
    return true;
  }
  if (v.IsUint()) {
    *out = std::to_string(v.GetUint());
    return true;
  }
  return false;
}

bool CheckNonNegative(const rapidjson::Value& obj, const char* name, int* out,
                      std::string* error) {
  if (!obj.HasMember(name) || obj[name].IsNull()) return true;
  int v = 0;
  if (!JsonGetInt(obj, name, &v) || v < 0) {
    if (error != nullptr) *error = std::string("bad_message: invalid ") + name;
    return false;
  }
  *out = v;
  return true;
}

}  // namespace

bool JsonGetString(const rapidjson::Value& obj, const char* name, std::string* out) {
  if (!obj.IsObject() || !obj.HasMember(name)) return false;
  const rapidjson::Value& v = obj[name];
  if (!v.IsString()) return false;
  out->assign(v.GetString(), v.GetStringLength());
  return true;
}

bool JsonGetInt(const rapidjson::Value& obj, const char* name, int* out) {
  if (!obj.IsObject() || !obj.HasMember(name)) return false;
  const rapidjson::Value& v = obj[name];
  if (v.IsInt()) {
    *out = v.GetInt();
    return true;
  }
  if (v.IsUint() && v.GetUint() <= static_cast<unsigned>(std::numeric_limits<int>::max())) {
    *out = static_cast<int>(v.GetUint());
    return true;
  }
  return false;
}

bool JsonGetInt64(const rapidjson::Value& obj, const char* name, int64_t* out) {
  if (!obj.IsObject() || !obj.HasMember(name)) return false;
  const rapidjson::Value& v = obj[name];
  if (v.IsInt64()) {
    *out = v.GetInt64();
    return true;
  }
  if (v.IsUint64() && v.GetUint64() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    *out = static_cast<int64_t>(v.GetUint64());
    return true;
  }
  if (v.IsInt()) {
    *out = v.GetInt();
    return true;
  }
  if (v.IsUint()) {
    *out = v.GetUint();
    return true;
  }
  return false;
}

bool JsonGetDouble(const rapidjson::Value& obj, const char* name, double* out) {
  if (!obj.IsObject() || !obj.HasMember(name) || !obj[name].IsNumber()) return false;
  *out = obj[name].GetDouble();
  return true;
}

bool JsonGetBool(const rapidjson::Value& obj, const char* name, bool* out) {
  if (!obj.IsObject() || !obj.HasMember(name) || !obj[name].IsBool()) return false;
  *out = obj[name].GetBool();
  return true;
}

bool ParseWsCommand(const char* data, std::size_t len, WsCommand* out, std::string* error) {
  if (data == nullptr || out == nullptr) return false;
  rapidjson::Document doc;
  doc.Parse(data, len);
  if (doc.HasParseError() || !doc.IsObject()) {
    if (error != nullptr) *error = doc.HasParseError() ? ParseErrorString(doc) : "bad_message: not an object";
    return false;
  }
  if (!doc.HasMember("type") || !doc["type"].IsString()) {
    if (error != nullptr) *error = "bad_message: missing type";
    return false;
  }
  out->type.assign(doc["type"].GetString(), doc["type"].GetStringLength());
  out->has_id = false;
  out->id = 0;
  if (doc.HasMember("id") && !doc["id"].IsNull()) {
    if (!JsonGetInt64(doc, "id", &out->id)) {
      if (error != nullptr) *error = "bad_message: id must be an integer";
      return false;
    }
    out->has_id = true;
  }
  out->doc.Swap(doc);
  return true;
}

bool ParseMqttCommand(const char* data, std::size_t len, MqttCommand* out, std::string* error) {
  if (data == nullptr || out == nullptr) return false;
  rapidjson::Document doc;
  doc.Parse(data, len);
  if (doc.HasParseError() || !doc.IsObject()) {
    if (error != nullptr) *error = doc.HasParseError() ? ParseErrorString(doc) : "bad_message: not an object";
    return false;
  }

  int v = 0;
  if (!JsonGetInt(doc, "v", &v) || v != 1) {
    if (error != nullptr) *error = "unsupported_version: envelope v";
    return false;
  }
  std::string type;
  if (!JsonGetString(doc, "type", &type) || type != "recorder_cmd") {
    if (error != nullptr) *error = "bad_message: type must be recorder_cmd";
    return false;
  }
  if (!doc.HasMember("id") || !ReadStringOrNumber(doc["id"], &out->id) || out->id.empty()) {
    if (error != nullptr) *error = "bad_message: missing id";
    return false;
  }
  if (!JsonGetString(doc, "action", &out->action)) {
    if (error != nullptr) *error = "bad_message: missing action";
    return false;
  }
  if (out->action != "start" && out->action != "stop" && out->action != "configure" &&
      out->action != "status" && out->action != "test_tone") {
    if (error != nullptr) *error = "unsupported_action: " + out->action;
    return false;
  }
  out->v = v;
  out->type = type;
  out->has_issued_at = JsonGetInt64(doc, "issued_at_ms", &out->issued_at_ms);
  out->has_expires_at = JsonGetInt64(doc, "expires_at_ms", &out->expires_at_ms);

  if (doc.HasMember("params") && !doc["params"].IsNull()) {
    if (!doc["params"].IsObject()) {
      if (error != nullptr) *error = "bad_message: params must be an object";
      return false;
    }
  }
  out->doc.Swap(doc);
  return true;
}

bool ParseAudioConfigJson(const rapidjson::Value& obj, AudioConfig* out, std::string* error) {
  if (!obj.IsObject() || out == nullptr) return false;

  std::string format;
  if (obj.HasMember("format") && !obj["format"].IsNull()) {
    if (!JsonGetString(obj, "format", &format) || !ParseAudioFormatName(format, &out->format)) {
      if (error != nullptr) *error = "unsupported_format: " + format;
      return false;
    }
  }
  if (obj.HasMember("sample_rate")) {
    if (!JsonGetInt(obj, "sample_rate", &out->sample_rate) || out->sample_rate <= 0) {
      if (error != nullptr) *error = "bad_message: sample_rate";
      return false;
    }
  }
  if (obj.HasMember("channels")) {
    if (!JsonGetInt(obj, "channels", &out->channels) || out->channels <= 0 || out->channels > 8) {
      if (error != nullptr) *error = "bad_message: channels";
      return false;
    }
  }
  if (obj.HasMember("frame_ms")) {
    if (!JsonGetInt(obj, "frame_ms", &out->frame_ms) || out->frame_ms <= 0 || out->frame_ms > 120) {
      if (error != nullptr) *error = "bad_message: frame_ms";
      return false;
    }
  }
  if (obj.HasMember("bits_per_sample")) {
    int bits = 0;
    if (!JsonGetInt(obj, "bits_per_sample", &bits) || (out->format == AudioFormat::kPcmS16le && bits != 16)) {
      if (error != nullptr) *error = "unsupported_format: bits_per_sample";
      return false;
    }
    out->bits_per_sample = bits;
  }
  if (obj.HasMember("endianness")) {
    std::string endianness;
    if (!JsonGetString(obj, "endianness", &endianness) || endianness != "little") {
      if (error != nullptr) *error = "unsupported_format: endianness";
      return false;
    }
  }
  if (obj.HasMember("interleaved")) {
    bool interleaved = true;
    if (!JsonGetBool(obj, "interleaved", &interleaved) || !interleaved) {
      if (error != nullptr) *error = "unsupported_format: must be interleaved";
      return false;
    }
    out->interleaved = true;
  }
  if (obj.HasMember("opus") && obj["opus"].IsObject()) {
    const rapidjson::Value& opus = obj["opus"];
    JsonGetString(opus, "encoder", &out->opus_encoder);
    if (opus.HasMember("bitrate")) {
      if (!JsonGetInt(opus, "bitrate", &out->opus_bitrate) || out->opus_bitrate <= 0) {
        if (error != nullptr) *error = "bad_message: opus.bitrate";
        return false;
      }
    }
    if (opus.HasMember("complexity")) {
      if (!JsonGetInt(opus, "complexity", &out->opus_complexity) ||
          out->opus_complexity < 0 || out->opus_complexity > 10) {
        if (error != nullptr) *error = "bad_message: opus.complexity";
        return false;
      }
    }
    if (opus.HasMember("vbr")) JsonGetBool(opus, "vbr", &out->opus_vbr);
    JsonGetString(opus, "application", &out->opus_application);
  }
  return true;
}

bool ParseCaptureConfigJson(const rapidjson::Value& obj, CaptureConfig* out, std::string* error) {
  if (!obj.IsObject() || out == nullptr) return false;

  if (obj.HasMember("mode")) {
    std::string mode;
    if (!JsonGetString(obj, "mode", &mode) || !ParseCaptureMode(mode, &out->mode)) {
      if (error != nullptr) *error = "unsupported_config: capture.mode";
      return false;
    }
  }
  if (!CheckNonNegative(obj, "pre_roll_ms", &out->pre_roll_ms, error)) return false;
  if (!CheckNonNegative(obj, "max_segment_ms", &out->max_segment_ms, error)) return false;
  if (!CheckNonNegative(obj, "segment_ms", &out->segment_ms, error)) return false;
  if (!CheckNonNegative(obj, "duration_ms", &out->duration_ms, error)) return false;
  if (out->mode == CaptureMode::kSingle && out->duration_ms <= 0) {
    if (error != nullptr) *error = "unsupported_config: single mode needs duration_ms";
    return false;
  }
  if (obj.HasMember("gain_db") && !obj["gain_db"].IsNull()) {
    double gain = 0;
    if (!JsonGetDouble(obj, "gain_db", &gain)) {
      if (error != nullptr) *error = "bad_message: capture.gain_db";
      return false;
    }
    out->gain_db = static_cast<int>(std::lround(gain));
  }

  if (obj.HasMember("vad") && obj["vad"].IsObject()) {
    const rapidjson::Value& vad = obj["vad"];
    if (vad.HasMember("engine")) {
      std::string engine;
      if (!JsonGetString(vad, "engine", &engine) || !ParseVadEngine(engine, &out->vad.engine)) {
        if (error != nullptr) *error = "unsupported_config: vad.engine";
        return false;
      }
    }
    if (!CheckNonNegative(vad, "onset_ms", &out->vad.onset_ms, error)) return false;
    if (!CheckNonNegative(vad, "silence_ms", &out->vad.silence_ms, error)) return false;
    if (!CheckNonNegative(vad, "min_speech_ms", &out->vad.min_speech_ms, error)) return false;
    if (!CheckNonNegative(vad, "guard_ms", &out->vad.guard_ms, error)) return false;
    if (vad.HasMember("threshold_db")) {
      double threshold = 0;
      if (!JsonGetDouble(vad, "threshold_db", &threshold) || threshold < 0) {
        if (error != nullptr) *error = "bad_message: vad.threshold_db";
        return false;
      }
      out->vad.threshold_db = threshold;
    }
    if (vad.HasMember("send_short_segments")) {
      JsonGetBool(vad, "send_short_segments", &out->vad.send_short_segments);
    }
  }
  return true;
}

bool ParseTelemetryConfigJson(const rapidjson::Value& obj, TelemetryConfig* out, std::string* error) {
  if (!obj.IsObject() || out == nullptr) return false;
  if (obj.HasMember("frame_stats")) JsonGetBool(obj, "frame_stats", &out->frame_stats);
  if (!CheckNonNegative(obj, "level_interval_ms", &out->level_interval_ms, error)) return false;
  if (!CheckNonNegative(obj, "stats_interval_ms", &out->stats_interval_ms, error)) return false;
  return true;
}

bool ParseToneJson(const rapidjson::Value& obj, ToneMetadata* out, std::string* error) {
  if (!obj.IsObject() || out == nullptr) return false;
  if (!JsonGetString(obj, "tone_id", &out->tone_id) || out->tone_id.empty()) {
    if (error != nullptr) *error = "bad_message: tone_id";
    return false;
  }
  if (!JsonGetString(obj, "waveform", &out->waveform)) out->waveform = "sine";
  if (out->waveform != "sine" && out->waveform != "square" && out->waveform != "sweep") {
    if (error != nullptr) *error = "unsupported_feature: waveform";
    return false;
  }
  if (out->waveform == "sweep") {
    if (!JsonGetDouble(obj, "start_hz", &out->start_hz) ||
        !JsonGetDouble(obj, "end_hz", &out->end_hz) ||
        out->start_hz <= 0 || out->end_hz <= 0) {
      if (error != nullptr) *error = "bad_message: sweep start_hz/end_hz";
      return false;
    }
    JsonGetString(obj, "sweep_mode", &out->sweep_mode);
    if (out->sweep_mode.empty()) out->sweep_mode = "linear";
    if (out->sweep_mode != "linear" && out->sweep_mode != "log") {
      if (error != nullptr) *error = "unsupported_feature: sweep_mode";
      return false;
    }
  } else {
    if (!JsonGetDouble(obj, "frequency_hz", &out->frequency_hz) || out->frequency_hz <= 0) {
      if (error != nullptr) *error = "bad_message: frequency_hz";
      return false;
    }
  }
  if (!JsonGetDouble(obj, "amplitude_dbfs", &out->amplitude_dbfs) || out->amplitude_dbfs > 0) {
    if (error != nullptr) *error = "bad_message: amplitude_dbfs";
    return false;
  }
  if (!JsonGetInt(obj, "duration_ms", &out->duration_ms) || out->duration_ms <= 0) {
    if (error != nullptr) *error = "bad_message: duration_ms";
    return false;
  }
  if (obj.HasMember("fade_ms")) JsonGetInt(obj, "fade_ms", &out->fade_ms);
  if (obj.HasMember("speaker_volume")) JsonGetInt(obj, "speaker_volume", &out->speaker_volume);
  if (obj.HasMember("loopback")) JsonGetBool(obj, "loopback", &out->loopback);
  JsonGetString(obj, "capture_path", &out->capture_path);
  return true;
}

}  // namespace recorder
