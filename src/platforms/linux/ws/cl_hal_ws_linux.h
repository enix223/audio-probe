#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cl_recorder_ws.h"

struct lws_context;
struct lws;

namespace recorder {

class WsClientLinux : public RecorderWs {
 public:
  WsClientLinux() = default;
  ~WsClientLinux() override;

  AvpErr Init(const WsConfig& config, const WsCallbacks& callbacks) override;
  AvpErr Connect() override;
  void Disconnect() override;
  bool IsConnected() const override;
  AvpErr SendText(const char* data, size_t len) override;
  AvpErr SendBinary(const uint8_t* data, size_t len) override;
  std::size_t PendingBytes() const override;

  // Public for the C-style libwebsockets callback.
  struct OutMessage {
    std::vector<uint8_t> data;  // payload, no LWS_PRE
    bool text = false;
  };

  int HandleCallback(lws* wsi, int reason, void* user, void* in, size_t len);
  void OnServiceWake();

 private:
  void ServiceLoop();
  bool PopNextMessage(OutMessage* out);
  bool WriteChunk(lws* wsi);
  bool HasPending() const;

  WsConfig config_;
  WsCallbacks callbacks_;
  lws_context* context_ = nullptr;
  lws* wsi_ = nullptr;
  std::atomic<int> state_{static_cast<int>(WsState::kDisconnected)};
  std::atomic<bool> running_{false};
  std::thread service_thread_;

  mutable std::mutex out_mu_;
  std::deque<OutMessage> out_queue_;
  std::size_t out_pending_bytes_ = 0;
  std::atomic<std::size_t> in_flight_bytes_{0};

  // Service-thread-only current message state.
  OutMessage current_;
  std::size_t current_offset_ = 0;

  // Receive reassembly (service thread only).
  std::vector<uint8_t> rx_buf_;
  bool rx_binary_ = false;

  bool destroyed_ = false;
};

}  // namespace recorder
