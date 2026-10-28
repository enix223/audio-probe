#pragma once

#ifdef CONFIG_IOT_PLATFORM_LN882H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "cl_recorder_mutex.h"
#include "cl_recorder_ws.h"

namespace recorder {

// LN882H WebSocket transport: lwIP BSD sockets + mbedTLS, reader and writer
// FreeRTOS tasks. SendText/SendBinary only enqueue a complete message; the
// writer task performs the socket/TLS write, so the app/capture task never
// blocks on the network. The reader task reassembles fragmented messages and
// dispatches complete text/binary payloads.
class WsClientLn882h : public RecorderWs {
 public:
  WsClientLn882h() = default;
  ~WsClientLn882h() override;

  AvpErr Init(const WsConfig& config, const WsCallbacks& callbacks) override;
  AvpErr Connect() override;
  void Disconnect() override;
  bool IsConnected() const override;
  AvpErr SendText(const char* data, size_t len) override;
  AvpErr SendBinary(const uint8_t* data, size_t len) override;
  std::size_t PendingBytes() const override;

 private:
  static constexpr uint8_t kOpContinuation = 0x0;
  static constexpr uint8_t kOpText = 0x1;
  static constexpr uint8_t kOpBinary = 0x2;
  static constexpr uint8_t kOpClose = 0x8;
  static constexpr uint8_t kOpPing = 0x9;
  static constexpr uint8_t kOpPong = 0xA;

  struct OutFrame {
    bool text = true;
    std::vector<uint8_t> data;
  };

  AvpErr ParseUrl(const std::string& url, std::string& host, int& port, std::string& path,
                  bool& tls);
  AvpErr TcpConnect();
  void TcpDisconnect();
  AvpErr TlsConnect();
  void TlsDisconnect();
  void ReleaseTransport();
  AvpErr WsHandshake();
  AvpErr RawSend(const uint8_t* data, size_t len);
  int RawRecv(uint8_t* data, size_t len, int timeout_ms);
  AvpErr SendFrame(uint8_t opcode, const uint8_t* data, size_t len);
  AvpErr RecvFrame();

  static void RecvTaskEntry(void* arg);
  static void SendTaskEntry(void* arg);
  void RecvTask();
  void SendTask();

  AvpErr Enqueue(bool text, const uint8_t* data, size_t len);

  WsConfig config_;
  WsCallbacks callbacks_;
  std::atomic<int> state_{static_cast<int>(WsState::kDisconnected)};

  int sock_fd_ = -1;
  bool is_tls_ = false;

  mutable RecorderMutex send_mu_;  // serializes whole frames and teardown
  void* ssl_ctx_ = nullptr;
  void* ssl_conf_ = nullptr;
  void* entropy_ = nullptr;
  void* ctr_drbg_ = nullptr;
  void* ca_chain_ = nullptr;

  void* recv_task_ = nullptr;  // OS_Thread_t*
  void* send_task_ = nullptr;
  std::atomic<bool> should_exit_{false};
  std::atomic<bool> recv_exited_{false};
  std::atomic<bool> send_exited_{false};

  mutable RecorderMutex queue_mu_;
  std::deque<OutFrame> out_;
  std::size_t out_bytes_ = 0;

  uint8_t recv_buf_[2048];
  std::vector<uint8_t> frag_;
  uint8_t frag_opcode_ = 0;
};

}  // namespace recorder

#endif  // CONFIG_IOT_PLATFORM_LN882H
