#include "cl_recorder_client.h"

#include <cstring>

#include "cl_recorder_commands.h"
#include "cl_recorder_messages.h"

namespace recorder {

namespace {
constexpr std::size_t kMaxPendingBytes = 256 * 1024;
constexpr int kMaxSendsPerFlush = 128;
}  // namespace

RecorderClient::RecorderClient(WsFactory factory, TxQueue::Config queue_cfg)
    : factory_(std::move(factory)), queue_(queue_cfg) {}

RecorderClient::~RecorderClient() { Disconnect(); }

bool RecorderClient::Connect(const WsConfig& config) {
  if (ws_) {
    if (ws_->IsConnected()) return false;
    // The previous transport failed asynchronously. Its service thread is
    // still alive; tear it down from this (app) thread before retrying.
    ws_->Disconnect();
    ws_.reset();
  }
  if (!factory_) return false;
  ws_ = factory_();
  if (!ws_) return false;

  closing_.store(false);
  WsCallbacks cb;
  cb.on_connected = [this]() { OnConnected(); };
  cb.on_disconnected = [this]() { OnDisconnected(); };
  cb.on_text = [this](const char* data, size_t len) { OnText(data, len); };
  cb.on_binary = [this](const uint8_t* data, size_t len) { OnBinary(data, len); };
  cb.on_error = [this](const char* error) { OnError(error); };

  if (ws_->Init(config, cb) != AvpErr::kOk) {
    ws_.reset();
    return false;
  }
  if (ws_->Connect() != AvpErr::kOk) {
    ws_->Disconnect();
    ws_.reset();
    return false;
  }
  return true;
}

void RecorderClient::Disconnect() {
  if (!ws_) return;
  closing_.store(true);
  connected_.store(false);
  ws_->Disconnect();
  ws_.reset();
  queue_.ClearAll();
  closing_.store(false);
}

bool RecorderClient::IsConnected() const { return connected_.load(); }

bool RecorderClient::TakeEvent(ClientEvent* out) {
  RecorderLockGuard lock(event_mu_);
  if (events_.empty()) return false;
  *out = std::move(events_.front());
  events_.pop_front();
  return true;
}

void RecorderClient::PushEvent(ClientEventType type, std::string text) {
  RecorderLockGuard lock(event_mu_);
  if (events_.size() > 256) events_.pop_front();
  events_.push_back(ClientEvent{type, std::move(text)});
}

void RecorderClient::OnConnected() {
  connected_.store(true);
  PushEvent(ClientEventType::kConnected, "");
}

void RecorderClient::OnDisconnected() {
  connected_.store(false);
  if (!closing_.load()) PushEvent(ClientEventType::kDisconnected, "");
}

void RecorderClient::OnError(const char* error) {
  if (!closing_.load()) PushEvent(ClientEventType::kError, error != nullptr ? error : "");
}

void RecorderClient::OnBinary(const uint8_t* data, size_t len) {
  (void)data;
  (void)len;
  // v1 has no server->device binary messages.
  PushEvent(ClientEventType::kError, "unexpected binary message from server");
}

void RecorderClient::OnText(const char* data, size_t len) {
  if (data == nullptr || len == 0) return;
  HandleText(data, len);
}

void RecorderClient::HandleText(const char* data, size_t len) {
  // Fast-path ping: parse only the envelope and the timing fields, then reply
  // directly. Ping must not wait behind queued audio or long commands.
  WsCommand cmd;
  std::string error;
  if (!ParseWsCommand(data, len, &cmd, &error)) {
    PushEvent(ClientEventType::kText, std::string(data, len));
    return;
  }
  if (cmd.type == "ping") {
    int64_t t0 = 0;
    JsonGetInt64(cmd.doc, "t0_ms", &t0);
    const int64_t t1 = clock_ != nullptr ? clock_->MonotonicMs() : 0;
    const std::string pong = BuildPong(cmd.has_id, cmd.id, t0, t1, false, 0);
    if (!SendImmediateControl(pong)) {
      send_errors_.fetch_add(1);
    }
    return;
  }
  PushEvent(ClientEventType::kText, std::string(data, len));
}

bool RecorderClient::EnqueueControl(std::string json) {
  TxRecord rec;
  rec.kind = TxRecord::Kind::kControl;
  rec.control = std::move(json);
  if (!queue_.PushControl(std::move(rec))) {
    control_rejected_.fetch_add(1);
    PushEvent(ClientEventType::kControlQueueFull, "");
    return false;
  }
  return true;
}

bool RecorderClient::EnqueueAudio(TxRecord rec) { return queue_.PushAudio(std::move(rec)); }

bool RecorderClient::SendImmediateControl(const std::string& json) {
  if (!connected_.load() || !ws_) return EnqueueControl(json);
  if (ws_->SendText(json.data(), json.size()) != AvpErr::kOk) {
    send_errors_.fetch_add(1);
    return false;
  }
  bytes_sent_.fetch_add(json.size());
  return true;
}

void RecorderClient::Flush() {
  if (!connected_.load() || !ws_) return;
  if (ws_->PendingBytes() > kMaxPendingBytes) return;

  for (int i = 0; i < kMaxSendsPerFlush; ++i) {
    TxRecord rec;
    if (!queue_.Pop(&rec)) return;

    AvpErr err = AvpErr::kErrFail;
    std::size_t sent_bytes = 0;
    if (rec.kind == TxRecord::Kind::kControl) {
      err = ws_->SendText(rec.control.data(), rec.control.size());
      sent_bytes = rec.control.size();
    } else {
      wire_.resize(protocol::kAudioHeaderSize + rec.payload.size());
      protocol::AudioHeader h;
      h.version = protocol::kProtocolVersion;
      h.type = protocol::kMessageTypeAudio;
      h.flags = rec.flags;
      h.format = rec.format;
      h.segment_id = rec.segment_id;
      h.seq = rec.seq;
      h.timestamp_ms = rec.timestamp_ms;
      h.samples = rec.samples;
      h.payload_len = static_cast<uint16_t>(rec.payload.size());
      h.rms_dbfs_x100 = rec.rms_dbfs_x100;
      h.peak = rec.peak;
      h.dc_offset = rec.dc_offset;
      h.vad_score = rec.vad_score;
      protocol::EncodeAudioHeader(h, wire_.data());
      if (!rec.payload.empty()) {
        std::memcpy(wire_.data() + protocol::kAudioHeaderSize, rec.payload.data(),
                    rec.payload.size());
      }
      err = ws_->SendBinary(wire_.data(), wire_.size());
      sent_bytes = wire_.size();
    }

    if (err != AvpErr::kOk) {
      // Keep the record (control) or drop it (audio) but never split it.
      if (rec.kind == TxRecord::Kind::kControl) {
        queue_.RequeueFront(std::move(rec));
      }
      send_errors_.fetch_add(1);
      PushEvent(ClientEventType::kError, "websocket send failed");
      return;
    }
    frames_sent_.fetch_add(rec.kind == TxRecord::Kind::kAudio ? 1 : 0);
    bytes_sent_.fetch_add(sent_bytes);
  }
}

void RecorderClient::ClearAudio() { queue_.ClearAudio(); }

void RecorderClient::ClearAll() { queue_.ClearAll(); }

void RecorderClient::ResetCounters() {
  frames_sent_.store(0);
  bytes_sent_.store(0);
  send_errors_.store(0);
  control_rejected_.store(0);
  queue_.ResetStats();
}

}  // namespace recorder
