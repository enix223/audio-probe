#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cl_recorder_types.h"

namespace recorder {

enum class WsState : uint8_t { kDisconnected, kConnecting, kConnected };

struct WsConfig {
  std::string url;  // ws://host:port/path or wss://...
  std::vector<std::pair<std::string, std::string>> headers;
  std::string ca_file;  // PEM bundle for WSS on Linux/macOS
  std::string ca_pem;   // inline PEM bundle for MCU builds
  bool allow_insecure = false;  // develop-only wss without CA verification
  int connect_timeout_ms = 10000;
};

struct WsCallbacks {
  std::function<void()> on_connected;
  std::function<void()> on_disconnected;
  std::function<void(const char* data, size_t len)> on_text;
  std::function<void(const uint8_t* data, size_t len)> on_binary;
  std::function<void(const char* error)> on_error;
};

// Platform WebSocket transport. Callbacks run on the transport's own service
// thread. SendText/SendBinary are non-blocking: they enqueue a complete
// message and the service thread performs the socket write. One call is
// exactly one WebSocket message (fragmentation is internal and does not
// change message boundaries).
class RecorderWs {
 public:
  virtual ~RecorderWs() = default;
  virtual AvpErr Init(const WsConfig& config, const WsCallbacks& callbacks) = 0;
  virtual AvpErr Connect() = 0;
  virtual void Disconnect() = 0;
  virtual bool IsConnected() const = 0;
  virtual AvpErr SendText(const char* data, size_t len) = 0;
  virtual AvpErr SendBinary(const uint8_t* data, size_t len) = 0;
  virtual std::size_t PendingBytes() const { return 0; }
};

}  // namespace recorder
