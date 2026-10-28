#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include "cl_recorder_mutex.h"
#include <string>
#include <vector>

#include "cl_recorder_types.h"
#include "cl_recorder_ws.h"
#include "cl_tx_queue.h"

namespace recorder {

class Clock;

enum class ClientEventType : uint8_t {
  kConnected,
  kDisconnected,
  kText,
  kError,
  kControlQueueFull,
};

struct ClientEvent {
  ClientEventType type = ClientEventType::kError;
  std::string text;
};

// Owns one WebSocket session at a time plus the bounded TX queue. Callbacks
// from the transport are turned into events consumed by the app task; the
// transport writer drains the queue from the app task via Flush(), and the
// platform transport performs the actual socket write on its service thread.
class RecorderClient {
 public:
  using WsFactory = std::function<std::unique_ptr<RecorderWs>()>;

  explicit RecorderClient(WsFactory factory, TxQueue::Config queue_cfg = TxQueue::Config());
  ~RecorderClient();

  void SetClock(Clock* clock) { clock_ = clock; }

  bool Connect(const WsConfig& config);
  void Disconnect();
  bool IsConnected() const;

  bool TakeEvent(ClientEvent* out);

  bool EnqueueControl(std::string json);
  bool EnqueueAudio(TxRecord rec);
  // Sends a control message directly through the transport (used for ping,
  // which must bypass the command/audio queue). Falls back to the queue when
  // the transport is not connected.
  bool SendImmediateControl(const std::string& json);

  // Non-blocking: sends queued records while the transport accepts them.
  void Flush();
  void ClearAudio();
  void ClearAll();
  void ResetCounters();

  TxQueue& queue() { return queue_; }
  const TxQueue& queue() const { return queue_; }

  uint32_t frames_sent() const { return frames_sent_.load(); }
  uint32_t bytes_sent() const { return bytes_sent_.load(); }
  uint32_t send_errors() const { return send_errors_.load(); }
  uint32_t control_rejected() const { return control_rejected_.load(); }

 private:
  void PushEvent(ClientEventType type, std::string text);
  void OnConnected();
  void OnDisconnected();
  void OnText(const char* data, size_t len);
  void OnBinary(const uint8_t* data, size_t len);
  void OnError(const char* error);
  void HandleText(const char* data, size_t len);

  WsFactory factory_;
  std::unique_ptr<RecorderWs> ws_;
  Clock* clock_ = nullptr;
  TxQueue queue_;

  mutable RecorderMutex event_mu_;
  std::deque<ClientEvent> events_;

  std::atomic<bool> connected_{false};
  std::atomic<bool> closing_{false};
  std::atomic<uint32_t> frames_sent_{0};
  std::atomic<uint32_t> bytes_sent_{0};
  std::atomic<uint32_t> send_errors_{0};
  std::atomic<uint32_t> control_rejected_{0};

  std::vector<uint8_t> wire_;
};

}  // namespace recorder
