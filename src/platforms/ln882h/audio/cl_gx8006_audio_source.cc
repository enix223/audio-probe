#include "cl_gx8006_audio_source.h"

#include <cstring>

#include "cl_recorder_protocol.h"

namespace recorder {

Gx8006AudioSource::Gx8006AudioSource(Gx8006Protocol* protocol, std::size_t queue_slots)
    : proto_(protocol), ring_(queue_slots == 0 ? 1 : queue_slots) {
  if (proto_ != nullptr) {
    proto_->SetOnMicData([this](const uint8_t* d, std::size_t n) { OnMicData(d, n); });
    proto_->SetOnMicStart([this]() { OnMicStart(); });
    proto_->SetOnMicStop([this]() { OnMicStop(); });
    proto_->SetOnChipRestart([this]() { OnChipRestart(); });
  }
}

AvpErr Gx8006AudioSource::Configure(const AudioConfig& cfg) {
  if (cfg.format != AudioFormat::kOpus) {
    // PCM is not advertised or accepted until the bench test proves it.
    return AvpErr::kErrNotSupported;
  }
  if (cfg.sample_rate != 16000 || cfg.channels != 1) return AvpErr::kErrNotSupported;
  if (cfg.frame_ms != 20 && cfg.frame_ms != 40 && cfg.frame_ms != 60) {
    return AvpErr::kErrNotSupported;
  }
  cfg_ = cfg;
  mic_cfg_.sample_rate = cfg.sample_rate;
  mic_cfg_.bits = 16;
  mic_cfg_.channels = cfg.channels;
  mic_cfg_.gain_db = gain_db_;
  mic_cfg_.opus = true;
  mic_cfg_.max_recv_bytes = static_cast<int>(kGxMaxRxPayload);
  return AvpErr::kOk;
}

AvpErr Gx8006AudioSource::Start() {
  if (proto_ == nullptr) return AvpErr::kErrNotInit;
  if (!proto_->ready()) {
    const AvpErr err = proto_->Handshake(mic_cfg_);
    if (err != AvpErr::kOk) return err;
  }
  if (proto_->ReArm() != AvpErr::kOk) return AvpErr::kErrFail;

  {
    RecorderLockGuard lock(mu_);
    head_ = tail_ = count_ = 0;
    pending_disc_ = false;
    disc_next_ = false;  // the first frame of a capture has no preceding gap
    samples_emitted_ = 0;
  }
  started_ = true;
  return AvpErr::kOk;
}

void Gx8006AudioSource::Stop() {
  started_ = false;
  if (proto_ != nullptr) proto_->SetMicrophone(false);
}

AudioFormat Gx8006AudioSource::NativeFormat() const { return AudioFormat::kOpus; }

AvpErr Gx8006AudioSource::Read(CaptureFrame& out, uint32_t timeout_ms) {
  out = CaptureFrame{};
  if (!started_) return AvpErr::kErrNotInit;

  bool restart = restart_pending_.exchange(false);
  if (proto_ != nullptr && proto_->TakeChipRestart()) restart = true;
  if (restart) {
    ++counters_.chip_restarts;
    if (proto_->Handshake(mic_cfg_) == AvpErr::kOk && proto_->ReArm() == AvpErr::kOk) {
      ++counters_.rehandshakes;
      RecorderLockGuard lock(mu_);
      disc_next_ = true;
    } else {
      return AvpErr::kErrFail;
    }
  }

  const GxProtocolStats before = proto_ != nullptr ? proto_->Stats() : GxProtocolStats{};
  (void)timeout_ms;  // the app polls with timeout 0

  for (int attempt = 0; attempt < 4; ++attempt) {
    Packet p;
    if (!Pop(&p)) {
      if (proto_ != nullptr) {
        const GxProtocolStats now = proto_->Stats();
        counters_.uart_overruns = now.rx_overflow;
        if (now.rx_overflow > before.rx_overflow) pending_disc_ = true;
      }
      return AvpErr::kErrTimeout;
    }
    const OpusTocInfo toc = ParseOpusToc(p.data, p.len);
    if (!toc.ok) {
      ++counters_.toc_errors;
      continue;
    }
    int samples = static_cast<int>(static_cast<int64_t>(toc.total_us) * cfg_.sample_rate / 1000000);
    if (samples <= 0) {
      samples = cfg_.sample_rate * cfg_.frame_ms / 1000;
    }
    current_ = p;
    out.data = current_.data;
    out.bytes = current_.len;
    out.samples = static_cast<std::size_t>(samples);
    out.pcm = nullptr;
    out.pcm_samples = 0;
    out.format = AudioFormat::kOpus;
    out.stats_valid = false;
    out.timestamp_ms =
        static_cast<uint32_t>(samples_emitted_ * 1000 / static_cast<uint64_t>(cfg_.sample_rate));
    samples_emitted_ += static_cast<uint64_t>(samples);
    RecorderLockGuard lock(mu_);
    out.discontinuity = p.discontinuity || pending_disc_;
    pending_disc_ = false;
    ++counters_.frames_read;
    return AvpErr::kOk;
  }
  return AvpErr::kErrTimeout;
}

void Gx8006AudioSource::OnMicData(const uint8_t* data, std::size_t len) {
  if (!started_ || data == nullptr || len == 0) return;
  RecorderLockGuard lock(mu_);
  ++counters_.packets;
  if (len > kPacketMax) {
    ++counters_.truncations;  // must stay zero; never silently truncate
    return;
  }
  if (count_ == ring_.size()) {
    // Drop the oldest packet, not the newest: the newest is the audio closest
    // to live. Mark the next queued packet discontinuous.
    tail_ = (tail_ + 1) % ring_.size();
    --count_;
    ++counters_.queue_drops;
    pending_disc_ = true;
  }
  Packet& p = ring_[head_];
  std::memcpy(p.data, data, len);
  p.len = static_cast<uint16_t>(len);
  p.discontinuity = disc_next_;
  disc_next_ = false;
  head_ = (head_ + 1) % ring_.size();
  ++count_;
}

void Gx8006AudioSource::OnMicStart() {
  RecorderLockGuard lock(mu_);
  ++counters_.mic_starts;
  // A chip upload session is a clean boundary, not a gap. START/STOP are
  // informational in server-VAD mode; only a STOP followed by new audio
  // indicates a possible gap.
  disc_next_ = false;
}

void Gx8006AudioSource::OnMicStop() {
  RecorderLockGuard lock(mu_);
  ++counters_.mic_stops;
  disc_next_ = true;
}

void Gx8006AudioSource::OnChipRestart() { restart_pending_ = true; }

bool Gx8006AudioSource::Pop(Packet* out) {
  RecorderLockGuard lock(mu_);
  if (count_ == 0) return false;
  *out = ring_[tail_];
  tail_ = (tail_ + 1) % ring_.size();
  --count_;
  return true;
}

SourceStats Gx8006AudioSource::Stats() const {
  GxProtocolStats ps;
  if (proto_ != nullptr) ps = proto_->Stats();
  SourceStats s;
  RecorderLockGuard lock(mu_);
  s.frames = counters_.frames_read;
  s.dropped = counters_.queue_drops;
  s.overruns = ps.rx_overflow;
  s.errors = counters_.toc_errors + counters_.truncations + ps.parse_errors + ps.checksum_errors;
  return s;
}

AvpErr Gx8006AudioSource::SetGain(int db) {
  if (db < 0 || db > 42) return AvpErr::kErrInvalidArg;
  gain_db_ = db;
  mic_cfg_.gain_db = db;
  if (proto_ != nullptr && proto_->ready()) return proto_->SetMicGain(db);
  return AvpErr::kOk;
}

Gx8006AudioSource::Counters Gx8006AudioSource::CountersSnapshot() const {
  RecorderLockGuard lock(mu_);
  return counters_;
}

}  // namespace recorder
