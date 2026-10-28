#include "cl_hal_ws_linux.h"

#include <libwebsockets.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace recorder {

namespace {

const char* kProtocolName = "recorder-v1";
constexpr std::size_t kWriteFragmentBytes = 2048;
constexpr std::size_t kMaxOutboundMessage = 1u << 20;
constexpr std::size_t kMaxInboundMessage = 4u << 20;

struct LwsUserData {
  WsClientLinux* self = nullptr;
};

struct ParsedUrl {
  std::string host;
  int port = 80;
  std::string path = "/";
  bool ssl = false;
};

ParsedUrl ParseUrl(const std::string& url) {
  ParsedUrl out;
  std::string rest = url;
  if (rest.rfind("wss://", 0) == 0) {
    out.ssl = true;
    out.port = 443;
    rest = rest.substr(6);
  } else if (rest.rfind("ws://", 0) == 0) {
    out.ssl = false;
    out.port = 80;
    rest = rest.substr(5);
  }
  const std::size_t slash = rest.find('/');
  std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : rest.substr(slash);
  const std::size_t colon = host_port.rfind(':');
  if (colon != std::string::npos) {
    out.host = host_port.substr(0, colon);
    try {
      out.port = std::stoi(host_port.substr(colon + 1));
    } catch (...) {
      out.port = out.ssl ? 443 : 80;
    }
  } else {
    out.host = host_port;
  }
  if (out.path.empty()) out.path = "/";
  return out;
}

int LwsCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
  auto* ud = static_cast<LwsUserData*>(user);
  if (ud == nullptr || ud->self == nullptr) return 0;
  return ud->self->HandleCallback(wsi, static_cast<int>(reason), user, in, len);
}

}  // namespace

WsClientLinux::~WsClientLinux() { Disconnect(); }

AvpErr WsClientLinux::Init(const WsConfig& config, const WsCallbacks& callbacks) {
  if (context_ != nullptr) return AvpErr::kErrAlreadyInit;
  config_ = config;
  callbacks_ = callbacks;

  lws_protocols protocols[2] = {};
  protocols[0].name = kProtocolName;
  protocols[0].callback = LwsCallback;
  protocols[0].per_session_data_size = sizeof(LwsUserData);
  protocols[0].rx_buffer_size = 65536;

  lws_context_creation_info info;
  std::memset(&info, 0, sizeof(info));
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  if (!config_.ca_file.empty()) info.client_ssl_ca_filepath = config_.ca_file.c_str();

  context_ = lws_create_context(&info);
  if (context_ == nullptr) return AvpErr::kErrFail;
  return AvpErr::kOk;
}

AvpErr WsClientLinux::Connect() {
  if (context_ == nullptr) return AvpErr::kErrNotInit;
  if (running_.load()) return AvpErr::kErrBusy;

  const ParsedUrl parsed = ParseUrl(config_.url);
  if (parsed.host.empty()) return AvpErr::kErrInvalidArg;

  state_.store(static_cast<int>(WsState::kConnecting));

  lws_client_connect_info ccinfo;
  std::memset(&ccinfo, 0, sizeof(ccinfo));
  ccinfo.context = context_;
  ccinfo.address = parsed.host.c_str();
  ccinfo.port = parsed.port;
  ccinfo.path = parsed.path.c_str();
  ccinfo.host = parsed.host.c_str();
  ccinfo.origin = parsed.host.c_str();
  ccinfo.protocol = kProtocolName;
  ccinfo.userdata = nullptr;
  if (parsed.ssl) ccinfo.ssl_connection = LCCSCF_USE_SSL;

  wsi_ = lws_client_connect_via_info(&ccinfo);
  if (wsi_ == nullptr) {
    state_.store(static_cast<int>(WsState::kDisconnected));
    return AvpErr::kErrFail;
  }
  auto* ud = static_cast<LwsUserData*>(lws_wsi_user(wsi_));
  if (ud != nullptr) ud->self = this;

  running_.store(true);
  service_thread_ = std::thread([this]() { ServiceLoop(); });
  return AvpErr::kOk;
}

void WsClientLinux::Disconnect() {
  const bool was_running = running_.exchange(false);
  if (context_ != nullptr) lws_cancel_service(context_);
  if (was_running && service_thread_.joinable()) service_thread_.join();
  if (!was_running && service_thread_.joinable()) service_thread_.join();

  if (context_ != nullptr) {
    lws_context_destroy(context_);
    context_ = nullptr;
  }
  wsi_ = nullptr;
  state_.store(static_cast<int>(WsState::kDisconnected));

  std::lock_guard<std::mutex> lock(out_mu_);
  out_queue_.clear();
  out_pending_bytes_ = 0;
  in_flight_bytes_.store(0);
  current_ = OutMessage{};
  current_offset_ = 0;
  rx_buf_.clear();
}

bool WsClientLinux::IsConnected() const {
  return state_.load() == static_cast<int>(WsState::kConnected);
}

AvpErr WsClientLinux::SendText(const char* data, size_t len) {
  if (!IsConnected() || context_ == nullptr) return AvpErr::kErrNotConnected;
  if (data == nullptr || len == 0 || len > kMaxOutboundMessage) return AvpErr::kErrInvalidArg;
  {
    std::lock_guard<std::mutex> lock(out_mu_);
    OutMessage msg;
    msg.text = true;
    msg.data.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + len);
    out_pending_bytes_ += len;
    out_queue_.push_back(std::move(msg));
  }
  lws_cancel_service(context_);
  return AvpErr::kOk;
}

AvpErr WsClientLinux::SendBinary(const uint8_t* data, size_t len) {
  if (!IsConnected() || context_ == nullptr) return AvpErr::kErrNotConnected;
  if (data == nullptr || len == 0 || len > kMaxOutboundMessage) return AvpErr::kErrInvalidArg;
  {
    std::lock_guard<std::mutex> lock(out_mu_);
    OutMessage msg;
    msg.text = false;
    msg.data.assign(data, data + len);
    out_pending_bytes_ += len;
    out_queue_.push_back(std::move(msg));
  }
  lws_cancel_service(context_);
  return AvpErr::kOk;
}

std::size_t WsClientLinux::PendingBytes() const {
  std::lock_guard<std::mutex> lock(out_mu_);
  return out_pending_bytes_ + in_flight_bytes_.load();
}

void WsClientLinux::ServiceLoop() {
  while (running_.load()) {
    lws_service(context_, 30);
    // lws_callback_on_writable() must run on the service thread. Polling the
    // bounded outbound queue here (instead of relying on the cross-thread
    // EVENT_WAIT_CANCELLED broadcast) keeps the writer correct and simple.
    if (wsi_ != nullptr && IsConnected() && HasPending()) {
      lws_callback_on_writable(wsi_);
    }
  }
}

bool WsClientLinux::HasPending() const {
  std::lock_guard<std::mutex> lock(out_mu_);
  return !out_queue_.empty() || in_flight_bytes_.load() > 0;
}

bool WsClientLinux::PopNextMessage(OutMessage* out) {
  std::lock_guard<std::mutex> lock(out_mu_);
  if (out_queue_.empty()) return false;
  *out = std::move(out_queue_.front());
  out_queue_.pop_front();
  out_pending_bytes_ -= out->data.size();
  return true;
}

bool WsClientLinux::WriteChunk(lws* wsi) {
  if (wsi == nullptr) return false;

  if (current_.data.empty()) {
    if (!PopNextMessage(&current_)) return true;
    current_offset_ = 0;
    in_flight_bytes_.store(current_.data.size());
  }
  if (current_offset_ >= current_.data.size()) {
    current_ = OutMessage{};
    current_offset_ = 0;
    in_flight_bytes_.store(0);
    lws_callback_on_writable(wsi);
    return true;
  }

  const std::size_t remaining = current_.data.size() - current_offset_;
  const std::size_t n = std::min(remaining, kWriteFragmentBytes);

  // lws_write requires LWS_PRE writable bytes before the payload pointer.
  std::vector<uint8_t> wire(LWS_PRE + n, 0);
  std::memcpy(wire.data() + LWS_PRE, current_.data.data() + current_offset_, n);
  const bool is_start = current_offset_ == 0;
  const bool is_end = current_offset_ + n == current_.data.size();
  const int flags = lws_write_ws_flags(current_.text ? LWS_WRITE_TEXT : LWS_WRITE_BINARY,
                                      is_start ? 1 : 0, is_end ? 1 : 0);
  const int rc = lws_write(wsi, wire.data() + LWS_PRE, n,
                           static_cast<lws_write_protocol>(flags));
  if (rc < 0 || static_cast<std::size_t>(rc) < n) return false;

  current_offset_ += n;
  in_flight_bytes_.store(current_.data.size() - current_offset_);
  if (current_offset_ < current_.data.size()) {
    lws_callback_on_writable(wsi);
  } else {
    current_ = OutMessage{};
    current_offset_ = 0;
    in_flight_bytes_.store(0);
    lws_callback_on_writable(wsi);
  }
  return true;
}

void WsClientLinux::OnServiceWake() {
  if (wsi_ != nullptr && IsConnected()) lws_callback_on_writable(wsi_);
}

int WsClientLinux::HandleCallback(lws* wsi, int reason, void* user, void* in, size_t len) {
  (void)user;

  switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      state_.store(static_cast<int>(WsState::kConnected));
      if (callbacks_.on_connected) callbacks_.on_connected();
      lws_callback_on_writable(wsi);
      break;

    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
      auto** p = reinterpret_cast<unsigned char**>(in);
      unsigned char* end = *p + len;
      for (const auto& h : config_.headers) {
        std::string name = h.first + ":";
        if (lws_add_http_header_by_name(wsi,
                                       reinterpret_cast<const unsigned char*>(name.c_str()),
                                       reinterpret_cast<const unsigned char*>(h.second.c_str()),
                                       static_cast<int>(h.second.size()), p, end)) {
          return -1;
        }
      }
      break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE:
      if (in != nullptr && len > 0) {
        if (rx_buf_.empty()) rx_binary_ = lws_frame_is_binary(wsi) != 0;
        if (rx_buf_.size() + len > kMaxInboundMessage) {
          if (callbacks_.on_error) callbacks_.on_error("inbound message too large");
          rx_buf_.clear();
          return -1;
        }
        rx_buf_.insert(rx_buf_.end(), static_cast<const uint8_t*>(in),
                       static_cast<const uint8_t*>(in) + len);
      }
      if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
        if (!rx_buf_.empty()) {
          if (rx_binary_) {
            if (callbacks_.on_binary) callbacks_.on_binary(rx_buf_.data(), rx_buf_.size());
          } else {
            if (callbacks_.on_text)
              callbacks_.on_text(reinterpret_cast<const char*>(rx_buf_.data()), rx_buf_.size());
          }
        }
        rx_buf_.clear();
      }
      break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
      if (!WriteChunk(wsi)) return -1;
      break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      OnServiceWake();
      break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      state_.store(static_cast<int>(WsState::kDisconnected));
      if (callbacks_.on_error) {
        callbacks_.on_error(in != nullptr ? static_cast<const char*>(in) : "connection error");
      }
      break;

    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
      if (in != nullptr && len >= 2) {
        const auto* bytes = static_cast<const unsigned char*>(in);
        const int code = (bytes[0] << 8) | bytes[1];
        const std::string msg =
            len > 2 ? std::string(reinterpret_cast<const char*>(bytes) + 2, len - 2) : std::string();
        if (callbacks_.on_error) {
          const std::string text = "server closed: code=" + std::to_string(code) +
                                   (msg.empty() ? "" : " reason=" + msg);
          callbacks_.on_error(text.c_str());
        }
      }
      break;
    }

    case LWS_CALLBACK_CLIENT_CLOSED:
      state_.store(static_cast<int>(WsState::kDisconnected));
      rx_buf_.clear();
      if (callbacks_.on_disconnected) callbacks_.on_disconnected();
      break;

    default:
      break;
  }
  return 0;
}

}  // namespace recorder
