#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "rapidjson/document.h"

#include "cl_recorder_types.h"

namespace recorder {

// One WS command from the server, with its document kept alive for field
// access. `id` is optional; the device echoes it when present.
struct WsCommand {
  std::string type;
  bool has_id = false;
  int64_t id = 0;
  rapidjson::Document doc;
};

bool ParseWsCommand(const char* data, std::size_t len, WsCommand* out, std::string* error);

// MQTT command envelope (docs/mqtt-control.md §4). `params` is an object or
// null; it is owned by the document.
struct MqttCommand {
  int v = 0;
  std::string type;
  std::string id;
  std::string action;
  bool has_issued_at = false;
  int64_t issued_at_ms = 0;
  bool has_expires_at = false;
  int64_t expires_at_ms = 0;
  rapidjson::Document doc;  // full envelope, including params

  const rapidjson::Value* Params() const {
    if (!doc.IsObject() || !doc.HasMember("params")) return nullptr;
    const rapidjson::Value& v = doc["params"];
    return v.IsObject() ? &v : nullptr;
  }
  bool HasParam(const char* name) const {
    const rapidjson::Value* p = Params();
    return p != nullptr && p->HasMember(name) && !(*p)[name].IsNull();
  }
};

bool ParseMqttCommand(const char* data, std::size_t len, MqttCommand* out, std::string* error);

// Field helpers shared by the command handlers. They never throw.
bool JsonGetString(const rapidjson::Value& obj, const char* name, std::string* out);
bool JsonGetInt(const rapidjson::Value& obj, const char* name, int* out);
bool JsonGetInt64(const rapidjson::Value& obj, const char* name, int64_t* out);
bool JsonGetDouble(const rapidjson::Value& obj, const char* name, double* out);
bool JsonGetBool(const rapidjson::Value& obj, const char* name, bool* out);

// Partial configuration parsers. Omitted fields leave the input unchanged.
bool ParseAudioConfigJson(const rapidjson::Value& obj, AudioConfig* out, std::string* error);
bool ParseCaptureConfigJson(const rapidjson::Value& obj, CaptureConfig* out, std::string* error);
bool ParseTelemetryConfigJson(const rapidjson::Value& obj, TelemetryConfig* out, std::string* error);
bool ParseToneJson(const rapidjson::Value& obj, ToneMetadata* out, std::string* error);

}  // namespace recorder
