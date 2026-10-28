#include "cl_hal_ws_ln882h.h"

#ifdef CONFIG_IOT_PLATFORM_LN882H

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "osal/osal.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/base64.h"
#include "mbedtls/error.h"
}

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "utils/cl_logger.h"

namespace recorder {

namespace {
constexpr const char* kTag = "ws";
constexpr std::size_t kRecvBufSize = 2048;
constexpr std::size_t kMaxQueuedBytes = 24 * 1024;
constexpr uint32_t kRecvTaskStack = 4096;
constexpr uint32_t kSendTaskStack = 3072;
}  // namespace

WsClientLn882h::~WsClientLn882h() { Disconnect(); }

AvpErr WsClientLn882h::Init(const WsConfig& config, const WsCallbacks& callbacks) {
  config_ = config;
  callbacks_ = callbacks;
  return AvpErr::kOk;
}

AvpErr WsClientLn882h::ParseUrl(const std::string& url, std::string& host, int& port,
                                std::string& path, bool& tls) {
  tls = false;
  std::size_t pos = 0;
  if (url.compare(0, 6, "wss://") == 0) {
    tls = true;
    pos = 6;
  } else if (url.compare(0, 5, "ws://") == 0) {
    pos = 5;
  } else {
    return AvpErr::kErrInvalidArg;
  }
  const std::size_t slash = url.find('/', pos);
  const std::string host_port =
      slash == std::string::npos ? url.substr(pos) : url.substr(pos, slash - pos);
  path = slash == std::string::npos ? "/" : url.substr(slash);
  const std::size_t colon = host_port.find(':');
  if (colon != std::string::npos) {
    host = host_port.substr(0, colon);
    port = std::atoi(host_port.substr(colon + 1).c_str());
  } else {
    host = host_port;
    port = tls ? 443 : 80;
  }
  return host.empty() ? AvpErr::kErrInvalidArg : AvpErr::kOk;
}

AvpErr WsClientLn882h::Connect() {
  if (state_.load() != static_cast<int>(WsState::kDisconnected)) return AvpErr::kErrBusy;
  state_.store(static_cast<int>(WsState::kConnecting));
  should_exit_.store(false);

  std::string host, path;
  int port = 0;
  bool tls = false;
  AvpErr err = ParseUrl(config_.url, host, port, path, tls);
  if (err != AvpErr::kOk) {
    state_.store(static_cast<int>(WsState::kDisconnected));
    return err;
  }
  is_tls_ = tls;

  if ((err = TcpConnect()) != AvpErr::kOk) {
    state_.store(static_cast<int>(WsState::kDisconnected));
    if (callbacks_.on_error) callbacks_.on_error("TCP connect failed");
    return err;
  }
  if (tls) {
    if ((err = TlsConnect()) != AvpErr::kOk) {
      ReleaseTransport();
      state_.store(static_cast<int>(WsState::kDisconnected));
      if (callbacks_.on_error) callbacks_.on_error("TLS connect failed");
      return err;
    }
  }
  if ((err = WsHandshake()) != AvpErr::kOk) {
    ReleaseTransport();
    state_.store(static_cast<int>(WsState::kDisconnected));
    if (callbacks_.on_error) callbacks_.on_error("WS handshake failed");
    return err;
  }

  state_.store(static_cast<int>(WsState::kConnected));
  if (callbacks_.on_connected) callbacks_.on_connected();

  recv_exited_.store(false);
  send_exited_.store(false);
  auto* rt = new OS_Thread_t;
  if (OS_ThreadCreate(rt, "ws_recv", RecvTaskEntry, this, OS_PRIORITY_NORMAL, kRecvTaskStack) !=
      OS_OK) {
    delete rt;
    recv_exited_.store(true);
    Disconnect();
    return AvpErr::kErrFail;
  }
  recv_task_ = rt;

  auto* st = new OS_Thread_t;
  if (OS_ThreadCreate(st, "ws_tx", SendTaskEntry, this, OS_PRIORITY_NORMAL, kSendTaskStack) !=
      OS_OK) {
    delete st;
    send_exited_.store(true);
    Disconnect();
    return AvpErr::kErrFail;
  }
  send_task_ = st;
  return AvpErr::kOk;
}

void WsClientLn882h::Disconnect() {
  if (state_.load() == static_cast<int>(WsState::kDisconnected) && recv_task_ == nullptr &&
      send_task_ == nullptr) {
    return;
  }
  should_exit_.store(true);
  if (state_.load() == static_cast<int>(WsState::kConnected)) {
    const uint8_t close_frame[2] = {0x03, 0xE8};  // 1000
    (void)SendFrame(kOpClose, close_frame, sizeof(close_frame));
  }
  if (sock_fd_ >= 0) {
    struct timeval tv = {0, 50000};
    setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  for (int i = 0; i < 50; ++i) {
    const bool recv_done = recv_task_ == nullptr || recv_exited_.load();
    const bool send_done = send_task_ == nullptr || send_exited_.load();
    if (recv_done && send_done) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (recv_task_ != nullptr) {
    delete static_cast<OS_Thread_t*>(recv_task_);
    recv_task_ = nullptr;
  }
  if (send_task_ != nullptr) {
    delete static_cast<OS_Thread_t*>(send_task_);
    send_task_ = nullptr;
  }
  ReleaseTransport();
  state_.store(static_cast<int>(WsState::kDisconnected));
  // Do not synthesize a disconnect callback here: a transport loss already
  // reports it from RecvTask, and an intentional Disconnect is suppressed by
  // RecorderClient's closing_ flag.
}

bool WsClientLn882h::IsConnected() const {
  return state_.load() == static_cast<int>(WsState::kConnected);
}

AvpErr WsClientLn882h::TcpConnect() {
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  std::string host, path;
  int port = 0;
  bool tls = false;
  if (ParseUrl(config_.url, host, port, path, tls) != AvpErr::kOk) return AvpErr::kErrInvalidArg;

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%d", port);
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
    LOG_E(kTag, "DNS resolve failed for %s", host.c_str());
    return AvpErr::kErrFail;
  }
  sock_fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock_fd_ < 0) {
    freeaddrinfo(res);
    return AvpErr::kErrFail;
  }
  struct timeval tv;
  tv.tv_sec = config_.connect_timeout_ms / 1000;
  tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;
  setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(sock_fd_, res->ai_addr, res->ai_addrlen) < 0) {
    close(sock_fd_);
    sock_fd_ = -1;
    freeaddrinfo(res);
    LOG_E(kTag, "TCP connect to %s:%d failed", host.c_str(), port);
    return AvpErr::kErrFail;
  }
  freeaddrinfo(res);
  LOG_I(kTag, "TCP connected to %s:%d", host.c_str(), port);
  return AvpErr::kOk;
}

void WsClientLn882h::TcpDisconnect() {
  if (sock_fd_ >= 0) {
    close(sock_fd_);
    sock_fd_ = -1;
  }
}

AvpErr WsClientLn882h::TlsConnect() {
  auto* ctx = new mbedtls_ssl_context;
  auto* conf = new mbedtls_ssl_config;
  auto* entropy = new mbedtls_entropy_context;
  auto* drbg = new mbedtls_ctr_drbg_context;
  auto* ca = new mbedtls_x509_crt;
  mbedtls_ssl_init(ctx);
  mbedtls_ssl_config_init(conf);
  mbedtls_entropy_init(entropy);
  mbedtls_ctr_drbg_init(drbg);
  mbedtls_x509_crt_init(ca);
  ssl_ctx_ = ctx;
  ssl_conf_ = conf;
  entropy_ = entropy;
  ctr_drbg_ = drbg;
  ca_chain_ = ca;

  int ret = mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, entropy, nullptr, 0);
  if (ret != 0) return AvpErr::kErrFail;
  ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) return AvpErr::kErrFail;
  mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, drbg);

  if (config_.allow_insecure) {
    LOG_W(kTag, "TLS certificate verification DISABLED (develop-only)");
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
  } else {
    if (config_.ca_pem.empty()) {
      LOG_E(kTag, "no CA configured for wss://; build a develop-only image to skip verification");
      return AvpErr::kErrNotSupported;
    }
    if (mbedtls_x509_crt_parse(ca, reinterpret_cast<const unsigned char*>(config_.ca_pem.data()),
                               config_.ca_pem.size() + 1) != 0) {
      LOG_E(kTag, "failed to parse the configured CA bundle");
      return AvpErr::kErrInvalidArg;
    }
    mbedtls_ssl_conf_ca_chain(conf, ca, nullptr);
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  }

  ret = mbedtls_ssl_setup(ctx, conf);
  if (ret != 0) return AvpErr::kErrFail;

  std::string host, path;
  int port = 0;
  bool tls = false;
  ParseUrl(config_.url, host, port, path, tls);
  mbedtls_ssl_set_hostname(ctx, host.c_str());
  mbedtls_ssl_set_bio(ctx, &sock_fd_, mbedtls_net_send, mbedtls_net_recv, nullptr);

  while ((ret = mbedtls_ssl_handshake(ctx)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      char err[96];
      mbedtls_strerror(ret, err, sizeof(err));
      LOG_E(kTag, "TLS handshake failed: -0x%04x %s", -ret, err);
      return AvpErr::kErrFail;
    }
  }
  LOG_I(kTag, "TLS connected");
  return AvpErr::kOk;
}

void WsClientLn882h::TlsDisconnect() {
  if (ssl_ctx_) {
    mbedtls_ssl_free(static_cast<mbedtls_ssl_context*>(ssl_ctx_));
    delete static_cast<mbedtls_ssl_context*>(ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
  if (ssl_conf_) {
    mbedtls_ssl_config_free(static_cast<mbedtls_ssl_config*>(ssl_conf_));
    delete static_cast<mbedtls_ssl_config*>(ssl_conf_);
    ssl_conf_ = nullptr;
  }
  if (ctr_drbg_) {
    mbedtls_ctr_drbg_free(static_cast<mbedtls_ctr_drbg_context*>(ctr_drbg_));
    delete static_cast<mbedtls_ctr_drbg_context*>(ctr_drbg_);
    ctr_drbg_ = nullptr;
  }
  if (entropy_) {
    mbedtls_entropy_free(static_cast<mbedtls_entropy_context*>(entropy_));
    delete static_cast<mbedtls_entropy_context*>(entropy_);
    entropy_ = nullptr;
  }
  if (ca_chain_) {
    mbedtls_x509_crt_free(static_cast<mbedtls_x509_crt*>(ca_chain_));
    delete static_cast<mbedtls_x509_crt*>(ca_chain_);
    ca_chain_ = nullptr;
  }
}

void WsClientLn882h::ReleaseTransport() {
  RecorderLockGuard lock(send_mu_);
  TlsDisconnect();
  TcpDisconnect();
}

AvpErr WsClientLn882h::RawSend(const uint8_t* data, size_t len) {
  if (is_tls_) {
    if (ssl_ctx_ == nullptr) return AvpErr::kErrNotConnected;
    auto* ctx = static_cast<mbedtls_ssl_context*>(ssl_ctx_);
    size_t sent = 0;
    while (sent < len) {
      const int ret = mbedtls_ssl_write(ctx, data + sent, len - sent);
      if (ret > 0) {
        sent += static_cast<size_t>(ret);
      } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        return AvpErr::kErrFail;
      }
    }
    return AvpErr::kOk;
  }
  if (sock_fd_ < 0) return AvpErr::kErrNotConnected;
  const ssize_t sent = send(sock_fd_, data, len, 0);
  return sent == static_cast<ssize_t>(len) ? AvpErr::kOk : AvpErr::kErrFail;
}

int WsClientLn882h::RawRecv(uint8_t* data, size_t len, int timeout_ms) {
  if (is_tls_ && ssl_ctx_ != nullptr) {
    const int ret = mbedtls_ssl_read(static_cast<mbedtls_ssl_context*>(ssl_ctx_), data, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -1;
    if (ret == MBEDTLS_ERR_NET_RECV_FAILED && errno == EAGAIN) return 0;
    if (ret < 0) {
      char err[96];
      mbedtls_strerror(ret, err, sizeof(err));
      LOG_E(kTag, "TLS read: -0x%04x %s (errno=%d)", -ret, err, errno);
      return -1;
    }
    return ret;
  }
  if (sock_fd_ < 0) return -1;
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  const ssize_t n = recv(sock_fd_, data, len, 0);
  if (n == 0) return -1;
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  return static_cast<int>(n);
}

AvpErr WsClientLn882h::WsHandshake() {
  std::string host, path;
  int port = 0;
  bool tls = false;
  ParseUrl(config_.url, host, port, path, tls);

  uint8_t key_raw[16];
  for (int i = 0; i < 16; ++i) key_raw[i] = static_cast<uint8_t>(rand() & 0xFF);
  unsigned char key_b64[32];
  size_t key_len = 0;
  if (mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_len, key_raw, sizeof(key_raw)) != 0) {
    return AvpErr::kErrFail;
  }

  std::string request;
  request.reserve(768);
  request += "GET " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: ";
  request.append(reinterpret_cast<char*>(key_b64), key_len);
  request += "\r\nSec-WebSocket-Version: 13\r\n";
  for (const auto& h : config_.headers) request += h.first + ": " + h.second + "\r\n";
  request += "\r\n";

  if (RawSend(reinterpret_cast<const uint8_t*>(request.data()), request.size()) != AvpErr::kOk) {
    return AvpErr::kErrFail;
  }

  size_t total = 0;
  bool done = false;
  while (!done && total < kRecvBufSize) {
    const int n = RawRecv(recv_buf_ + total, 1, config_.connect_timeout_ms);
    if (n <= 0) return AvpErr::kErrTimeout;
    total += static_cast<size_t>(n);
    if (total >= 4 && recv_buf_[total - 4] == '\r' && recv_buf_[total - 3] == '\n' &&
        recv_buf_[total - 2] == '\r' && recv_buf_[total - 1] == '\n') {
      done = true;
    }
  }
  if (!done || memcmp(recv_buf_, "HTTP/1.1 101", 12) != 0) {
    size_t line = 0;
    while (line < total && line < 127 && recv_buf_[line] != '\r') ++line;
    LOG_E(kTag, "handshake rejected: %.*s", static_cast<int>(line), recv_buf_);
    return AvpErr::kErrFail;
  }
  LOG_I(kTag, "WebSocket connected");
  return AvpErr::kOk;
}

AvpErr WsClientLn882h::SendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
  RecorderLockGuard lock(send_mu_);
  uint8_t header[14];
  size_t hlen = 0;
  header[hlen++] = static_cast<uint8_t>(0x80 | (opcode & 0x0F));
  uint8_t mask[4];
  for (int i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(rand() & 0xFF);
  if (len < 126) {
    header[hlen++] = static_cast<uint8_t>(0x80 | len);
  } else if (len < 65536) {
    header[hlen++] = 0x80 | 126;
    header[hlen++] = static_cast<uint8_t>(len >> 8);
    header[hlen++] = static_cast<uint8_t>(len);
  } else {
    header[hlen++] = 0x80 | 127;
    for (int i = 7; i >= 0; --i) header[hlen++] = static_cast<uint8_t>(len >> (i * 8));
  }
  memcpy(header + hlen, mask, 4);
  hlen += 4;
  AvpErr err = RawSend(header, hlen);
  if (err != AvpErr::kOk) return err;
  if (len == 0) return AvpErr::kOk;

  static constexpr size_t kStackBuf = 512;
  uint8_t stack_buf[kStackBuf];
  uint8_t* masked = len <= kStackBuf ? stack_buf : static_cast<uint8_t*>(malloc(len));
  if (masked == nullptr) return AvpErr::kErrNoMem;
  for (size_t i = 0; i < len; ++i) masked[i] = data[i] ^ mask[i % 4];
  err = RawSend(masked, len);
  if (masked != stack_buf) free(masked);
  return err;
}

AvpErr WsClientLn882h::Enqueue(bool text, const uint8_t* data, size_t len) {
  if (state_.load() != static_cast<int>(WsState::kConnected)) return AvpErr::kErrNotConnected;
  if (data == nullptr || len == 0) return AvpErr::kErrInvalidArg;
  RecorderLockGuard lock(queue_mu_);
  if (out_bytes_ + len > kMaxQueuedBytes) return AvpErr::kErrBusy;
  OutFrame frame;
  frame.text = text;
  frame.data.assign(data, data + len);
  out_bytes_ += len;
  out_.push_back(std::move(frame));
  return AvpErr::kOk;
}

AvpErr WsClientLn882h::SendText(const char* data, size_t len) {
  return Enqueue(true, reinterpret_cast<const uint8_t*>(data), len);
}

AvpErr WsClientLn882h::SendBinary(const uint8_t* data, size_t len) {
  return Enqueue(false, data, len);
}

std::size_t WsClientLn882h::PendingBytes() const {
  RecorderLockGuard lock(queue_mu_);
  return out_bytes_;
}

AvpErr WsClientLn882h::RecvFrame() {
  uint8_t hdr[2];
  size_t got = 0;
  while (got < 2) {
    const int n = RawRecv(hdr + got, 2 - got, 100);
    if (n < 0) return AvpErr::kErrFail;
    if (n == 0) {
      if (should_exit_.load()) return AvpErr::kErrFail;
      return AvpErr::kErrTimeout;
    }
    got += static_cast<size_t>(n);
  }
  const bool fin = (hdr[0] & 0x80) != 0;
  const uint8_t opcode = hdr[0] & 0x0F;
  const bool masked = (hdr[1] & 0x80) != 0;
  uint64_t len = hdr[1] & 0x7F;
  if (len == 126) {
    uint8_t ext[2];
    if (RawRecv(ext, 2, 100) <= 0) return AvpErr::kErrFail;
    len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    uint8_t ext[8];
    if (RawRecv(ext, 8, 100) <= 0) return AvpErr::kErrFail;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
  }
  uint8_t mask[4] = {};
  if (masked && RawRecv(mask, 4, 100) <= 0) return AvpErr::kErrFail;
  if (len > kRecvBufSize) {
    LOG_E(kTag, "frame too large: %u", static_cast<unsigned>(len));
    return AvpErr::kErrFail;
  }
  size_t total = 0;
  while (total < len) {
    const int n = RawRecv(recv_buf_ + total, static_cast<size_t>(len) - total, 100);
    if (n <= 0) return AvpErr::kErrFail;
    total += static_cast<size_t>(n);
  }
  if (masked) {
    for (size_t i = 0; i < total; ++i) recv_buf_[i] ^= mask[i % 4];
  }

  if (opcode == kOpPing) {
    (void)SendFrame(kOpPong, recv_buf_, total);
    return AvpErr::kOk;
  }
  if (opcode == kOpPong) return AvpErr::kOk;
  if (opcode == kOpClose) {
    state_.store(static_cast<int>(WsState::kDisconnected));
    return AvpErr::kErrFail;
  }
  if (opcode == kOpText || opcode == kOpBinary) {
    if (fin) {
      if (callbacks_.on_text && opcode == kOpText) {
        callbacks_.on_text(reinterpret_cast<const char*>(recv_buf_), total);
      } else if (callbacks_.on_binary && opcode == kOpBinary) {
        callbacks_.on_binary(recv_buf_, total);
      }
    } else {
      frag_.clear();
      frag_.insert(frag_.end(), recv_buf_, recv_buf_ + total);
      frag_opcode_ = opcode;
    }
    return AvpErr::kOk;
  }
  if (opcode == kOpContinuation) {
    if (frag_.size() + total > kRecvBufSize) return AvpErr::kErrFail;
    frag_.insert(frag_.end(), recv_buf_, recv_buf_ + total);
    if (fin) {
      if (frag_opcode_ == kOpText && callbacks_.on_text) {
        callbacks_.on_text(reinterpret_cast<const char*>(frag_.data()), frag_.size());
      } else if (frag_opcode_ == kOpBinary && callbacks_.on_binary) {
        callbacks_.on_binary(frag_.data(), frag_.size());
      }
      frag_.clear();
    }
  }
  return AvpErr::kOk;
}

void WsClientLn882h::RecvTaskEntry(void* arg) {
  static_cast<WsClientLn882h*>(arg)->RecvTask();
}

void WsClientLn882h::SendTaskEntry(void* arg) {
  static_cast<WsClientLn882h*>(arg)->SendTask();
}

void WsClientLn882h::RecvTask() {
  bool lost = false;
  while (!should_exit_.load() && state_.load() == static_cast<int>(WsState::kConnected)) {
    const AvpErr err = RecvFrame();
    if (err != AvpErr::kOk && err != AvpErr::kErrTimeout) {
      lost = true;
      break;
    }
  }
  if (lost) {
    state_.store(static_cast<int>(WsState::kDisconnected));
    ReleaseTransport();
    if (callbacks_.on_disconnected) callbacks_.on_disconnected();
  }
  recv_exited_.store(true);
  OS_ThreadDelete(nullptr);
}

void WsClientLn882h::SendTask() {
  while (!should_exit_.load()) {
    OutFrame frame;
    bool have = false;
    {
      RecorderLockGuard lock(queue_mu_);
      if (!out_.empty()) {
        frame = std::move(out_.front());
        out_.pop_front();
        out_bytes_ -= frame.data.size();
        have = true;
      }
    }
    if (!have) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const AvpErr err = SendFrame(frame.text ? kOpText : kOpBinary, frame.data.data(),
                                 frame.data.size());
    if (err != AvpErr::kOk) {
      state_.store(static_cast<int>(WsState::kDisconnected));
      if (callbacks_.on_error) callbacks_.on_error("websocket send failed");
      break;
    }
  }
  send_exited_.store(true);
  OS_ThreadDelete(nullptr);
}

}  // namespace recorder

#endif  // CONFIG_IOT_PLATFORM_LN882H
