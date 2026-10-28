#include "cl_gx8006_protocol.h"

#include <cstring>

namespace recorder {

namespace {

uint32_t GxReadU32Be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void GxWriteU32Be(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

}  // namespace

void Gx8006Protocol::Init(WriteFn write, FcReadyFn fc_ready, DelayFn delay) {
  write_ = std::move(write);
  fc_ready_ = std::move(fc_ready);
  delay_ = std::move(delay);
}

std::size_t Gx8006Protocol::Avail() const {
  return (head_ - tail_) & (kGxRxRingSize - 1);
}

uint8_t Gx8006Protocol::Peek(std::size_t offset) const {
  return rx_[(tail_ + offset) & (kGxRxRingSize - 1)];
}

void Gx8006Protocol::CopyOut(std::size_t offset, uint8_t* dst, std::size_t n) const {
  for (std::size_t i = 0; i < n; ++i) dst[i] = Peek(offset + i);
}

void Gx8006Protocol::Drop(std::size_t n) { tail_ = (tail_ + n) & (kGxRxRingSize - 1); }

void Gx8006Protocol::Input(const uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) return;
  RecorderLockGuard lock(mu_);
  stats_.rx_bytes += static_cast<uint32_t>(len);
  for (std::size_t i = 0; i < len; ++i) {
    const std::size_t next = (head_ + 1) & (kGxRxRingSize - 1);
    if (next == tail_) {
      ++stats_.rx_overflow;
      return;  // do not overwrite unparsed bytes; the parser will resync
    }
    rx_[head_] = data[i];
    head_ = next;
  }
}

void Gx8006Protocol::Drain() {
  RecorderLockGuard lock(mu_);
  tail_ = head_;
}

void Gx8006Protocol::Process() {
  RecorderLockGuard lock(mu_);
  for (;;) {
    if (Avail() < 7) return;
    if (Peek(0) != kGxHead0 || Peek(1) != kGxHead1) {
      Drop(1);
      ++stats_.parse_errors;
      continue;
    }
    const uint8_t version = Peek(2);
    const uint8_t cmd = Peek(3);
    if (version != kGxVersionResponse || cmd != kGxCmd) {
      Drop(1);
      ++stats_.parse_errors;
      continue;
    }
    const uint16_t payload_len = static_cast<uint16_t>((Peek(4) << 8) | Peek(5));
    if (payload_len > kGxMaxRxPayload) {
      Drop(1);
      ++stats_.parse_errors;
      continue;
    }
    const std::size_t total = kGxHeaderSize + payload_len + 1;
    if (Avail() < total) return;

    uint8_t frame[kGxMaxRxFrame];
    CopyOut(0, frame, total);
    uint8_t sum = 0;
    for (std::size_t i = 0; i < kGxHeaderSize + payload_len; ++i) sum += frame[i];
    if (sum != frame[kGxHeaderSize + payload_len]) {
      ++stats_.checksum_errors;
      Drop(1);
      continue;
    }
    Drop(total);
    ++stats_.frames;
    Dispatch(frame + kGxHeaderSize, payload_len);
  }
}

void Gx8006Protocol::Dispatch(const uint8_t* payload, std::size_t len) {
  if (len < 1) return;
  switch (static_cast<GxSubCmd>(payload[0])) {
    case GxSubCmd::kVersionQuery: {
      if (len < 4) return;
      const uint8_t query_type = payload[1];
      if (query_type == 1) {
        version_seen_.store(true);
      } else {
        ++stats_.chip_restarts;
        chip_restart_.store(true);
        if (on_chip_restart_) on_chip_restart_();
      }
      break;
    }
    case GxSubCmd::kGetMicData: {
      if (len < 2) return;
      const uint8_t indicator = payload[1];
      if (indicator == static_cast<uint8_t>(GxIndicator::kStart)) {
        ++stats_.mic_starts;
        if (on_mic_start_) on_mic_start_();
      } else if (indicator == static_cast<uint8_t>(GxIndicator::kMid)) {
        if (len > 2) {
          ++stats_.mic_frames;
          if (on_mic_data_) on_mic_data_(payload + 2, len - 2);
        }
      } else if (indicator == static_cast<uint8_t>(GxIndicator::kStop)) {
        ++stats_.mic_stops;
        if (on_mic_stop_) on_mic_stop_();
      }
      break;
    }
    case GxSubCmd::kGetOfflineWakeup:
      // KWS is never armed by the recorder. Count the event and ignore it.
      ++stats_.wake_events;
      break;
    case GxSubCmd::kGetTimeout:
      if (on_session_timeout_) on_session_timeout_();
      break;
    case GxSubCmd::kEventNotify:
    case GxSubCmd::kEventNotifyV2:
      break;
    default:
      ++stats_.unknown_frames;
      break;
  }
}

AvpErr Gx8006Protocol::SendFrame(const uint8_t* payload, std::size_t len) {
  if (!write_) return AvpErr::kErrNotInit;
  if (payload == nullptr || len == 0 || len > kGxMaxTxPayload) return AvpErr::kErrInvalidArg;
  uint8_t frame[kGxHeaderSize + kGxMaxTxPayload + 1] = {};
  frame[0] = kGxHead0;
  frame[1] = kGxHead1;
  frame[2] = kGxVersionRequest;
  frame[3] = kGxCmd;
  frame[4] = static_cast<uint8_t>(len >> 8);
  frame[5] = static_cast<uint8_t>(len);
  std::memcpy(frame + kGxHeaderSize, payload, len);
  uint8_t sum = 0;
  for (std::size_t i = 0; i < kGxHeaderSize + len; ++i) sum += frame[i];
  frame[kGxHeaderSize + len] = sum;
  const int n = write_(frame, kGxHeaderSize + len + 1);
  return n == static_cast<int>(kGxHeaderSize + len + 1) ? AvpErr::kOk : AvpErr::kErrFail;
}

AvpErr Gx8006Protocol::SendVersionQuery() {
  const uint8_t payload[] = {static_cast<uint8_t>(GxSubCmd::kVersionQuery)};
  return SendFrame(payload, sizeof(payload));
}

AvpErr Gx8006Protocol::SendMicConfig() {
  uint8_t payload[13] = {};
  payload[0] = static_cast<uint8_t>(GxSubCmd::kSetMicConfig);
  GxWriteU32Be(payload + 1, static_cast<uint32_t>(cfg_.sample_rate));
  payload[5] = static_cast<uint8_t>(cfg_.bits);
  payload[6] = static_cast<uint8_t>(cfg_.channels);
  payload[7] = static_cast<uint8_t>(cfg_.gain_db);
  payload[8] = static_cast<uint8_t>(cfg_.opus ? GxAudioFormat::kOpus : GxAudioFormat::kPcm);
  GxWriteU32Be(payload + 9, static_cast<uint32_t>(cfg_.max_recv_bytes));
  return SendFrame(payload, sizeof(payload));
}

AvpErr Gx8006Protocol::SendSpkConfig() {
  uint8_t payload[12] = {};
  payload[0] = static_cast<uint8_t>(GxSubCmd::kSetSpkConfig);
  GxWriteU32Be(payload + 1, 16000);
  payload[5] = 16;
  payload[6] = static_cast<uint8_t>(volume_);
  payload[7] = static_cast<uint8_t>(GxAudioFormat::kOpus);
  GxWriteU32Be(payload + 8, static_cast<uint32_t>(kGxMaxSpkData));
  return SendFrame(payload, sizeof(payload));
}

AvpErr Gx8006Protocol::SendOffline(GxOfflineSubCmd sub, uint8_t value) {
  uint8_t payload[3] = {static_cast<uint8_t>(GxSubCmd::kSetOfflineConfig),
                        static_cast<uint8_t>(sub), value};
  return SendFrame(payload, sizeof(payload));
}

bool Gx8006Protocol::WaitFlag(const std::atomic<bool>& flag, uint32_t timeout_ms) {
  for (uint32_t elapsed = 0; elapsed <= timeout_ms; elapsed += 5) {
    if (flag.load()) return true;
    if (delay_) delay_(5);
    Process();
  }
  return flag.load();
}

AvpErr Gx8006Protocol::Handshake(const GxMicConfig& cfg) {
  cfg_ = cfg;
  ready_ = false;
  version_seen_.store(false);
  chip_restart_.store(false);

  if (SendVersionQuery() != AvpErr::kOk) return AvpErr::kErrFail;
  if (!WaitFlag(version_seen_, 500)) return AvpErr::kErrTimeout;

  if (SendMicConfig() != AvpErr::kOk) return AvpErr::kErrFail;
  if (SendSpkConfig() != AvpErr::kOk) return AvpErr::kErrFail;

  // Continuous pickup: 60 s re-arm period, VAD off, microphone on. No KWS.
  if (SendOffline(GxOfflineSubCmd::kSetPickupPeriod, 60) != AvpErr::kOk) return AvpErr::kErrFail;
  if (SendOffline(GxOfflineSubCmd::kVad, 0) != AvpErr::kOk) return AvpErr::kErrFail;
  if (SendOffline(GxOfflineSubCmd::kMic, 1) != AvpErr::kOk) return AvpErr::kErrFail;

  ready_ = true;
  return AvpErr::kOk;
}

AvpErr Gx8006Protocol::SetMicrophone(bool on) {
  return SendOffline(GxOfflineSubCmd::kMic, on ? 1 : 0);
}

AvpErr Gx8006Protocol::SetVad(uint8_t value) {
  return SendOffline(GxOfflineSubCmd::kVad, value);
}

AvpErr Gx8006Protocol::SetPickupPeriod(uint8_t seconds) {
  return SendOffline(GxOfflineSubCmd::kSetPickupPeriod, seconds);
}

AvpErr Gx8006Protocol::SetMicGain(int db) {
  cfg_.gain_db = db;
  return SendMicConfig();
}

AvpErr Gx8006Protocol::ReArm() {
  if (SendOffline(GxOfflineSubCmd::kSetPickupPeriod, 60) != AvpErr::kOk) return AvpErr::kErrFail;
  if (SendOffline(GxOfflineSubCmd::kVad, 0) != AvpErr::kOk) return AvpErr::kErrFail;
  return SendOffline(GxOfflineSubCmd::kMic, 1);
}

AvpErr Gx8006Protocol::SendSpeakerStart() {
  spk_pkt_ = 0;
  uint8_t payload[6] = {static_cast<uint8_t>(GxSubCmd::kSetSpkData),
                        static_cast<uint8_t>(GxIndicator::kStart)};
  GxWriteU32Be(payload + 2, 0xFFFFFFFFu);
  return SendFrame(payload, sizeof(payload));
}

AvpErr Gx8006Protocol::SendSpeakerData(const uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0 || len > kGxMaxSpkData) return AvpErr::kErrInvalidArg;
  uint8_t payload[6 + kGxMaxSpkData];
  payload[0] = static_cast<uint8_t>(GxSubCmd::kSetSpkData);
  payload[1] = static_cast<uint8_t>(GxIndicator::kMid);
  GxWriteU32Be(payload + 2, spk_pkt_++);
  std::memcpy(payload + 6, data, len);
  return SendFrame(payload, 6 + len);
}

AvpErr Gx8006Protocol::SendSpeakerStop() {
  uint8_t payload[6] = {static_cast<uint8_t>(GxSubCmd::kSetSpkData),
                        static_cast<uint8_t>(GxIndicator::kStop)};
  GxWriteU32Be(payload + 2, 0xFFFFFFFFu);
  return SendFrame(payload, sizeof(payload));
}

bool Gx8006Protocol::WaitFlowControl(uint32_t timeout_ms) {
  if (!fc_ready_) return true;
  for (uint32_t elapsed = 0; elapsed <= timeout_ms; ++elapsed) {
    if (fc_ready_()) return true;
    if (delay_) delay_(1);
  }
  return fc_ready_();
}

bool Gx8006Protocol::TakeChipRestart() { return chip_restart_.exchange(false); }

GxProtocolStats Gx8006Protocol::Stats() const {
  RecorderLockGuard lock(mu_);
  return stats_;
}

}  // namespace recorder
