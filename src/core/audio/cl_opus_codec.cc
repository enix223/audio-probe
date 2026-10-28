#include "cl_opus_codec.h"

#if defined(RECORDER_MCU_LN882H)

// The LN882H first milestone uses the GX8006's chip-owned Opus encoder and
// never links a host Opus library. Keep the class linkable but inert.
namespace recorder {

OpusEncoder::~OpusEncoder() = default;
AvpErr OpusEncoder::Init(const AudioConfig&) { return AvpErr::kErrNotSupported; }
void OpusEncoder::Reset() { frame_samples_ = 0; }
AvpErr OpusEncoder::Encode(const int16_t*, std::size_t, std::vector<uint8_t>& out) {
  out.clear();
  return AvpErr::kErrNotSupported;
}

}  // namespace recorder

#else

#include <opus.h>

#include <algorithm>
#include <cstring>

namespace recorder {

namespace {
constexpr std::size_t kMaxPacketBytes = 4000;  // libopus upper bound for 120 ms
}  // namespace

OpusEncoder::~OpusEncoder() {
  if (encoder_ != nullptr) opus_encoder_destroy(static_cast<::OpusEncoder*>(encoder_));
}

AvpErr OpusEncoder::Init(const AudioConfig& cfg) {
  Reset();
  if (cfg.format != AudioFormat::kOpus) return AvpErr::kErrInvalidArg;
  if (cfg.sample_rate != 8000 && cfg.sample_rate != 12000 && cfg.sample_rate != 16000 &&
      cfg.sample_rate != 24000 && cfg.sample_rate != 48000) {
    return AvpErr::kErrNotSupported;
  }
  if (cfg.channels != 1 && cfg.channels != 2) return AvpErr::kErrNotSupported;
  if (cfg.frame_ms <= 0) return AvpErr::kErrInvalidArg;

  int err = OPUS_OK;
  ::OpusEncoder* enc = opus_encoder_create(cfg.sample_rate, cfg.channels,
                                           OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK || enc == nullptr) return AvpErr::kErrFail;

  int bandwidth = OPUS_BANDWIDTH_NARROWBAND;
  if (cfg.sample_rate >= 48000) {
    bandwidth = OPUS_BANDWIDTH_FULLBAND;
  } else if (cfg.sample_rate >= 24000) {
    bandwidth = OPUS_BANDWIDTH_SUPERWIDEBAND;
  } else if (cfg.sample_rate >= 16000) {
    bandwidth = OPUS_BANDWIDTH_WIDEBAND;
  }
  opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(bandwidth));
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(cfg.opus_bitrate > 0 ? cfg.opus_bitrate : 32000));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(cfg.opus_complexity >= 0 ? cfg.opus_complexity : 5));
  opus_encoder_ctl(enc, OPUS_SET_VBR(cfg.opus_vbr ? 1 : 0));
  opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  encoder_ = enc;
  cfg_ = cfg;
  frame_samples_ = static_cast<std::size_t>(cfg.sample_rate) *
                   static_cast<std::size_t>(cfg.frame_ms) / 1000;
  return AvpErr::kOk;
}

void OpusEncoder::Reset() {
  if (encoder_ != nullptr) {
    opus_encoder_destroy(static_cast<::OpusEncoder*>(encoder_));
    encoder_ = nullptr;
  }
  frame_samples_ = 0;
  dropped_samples_ = 0;
}

AvpErr OpusEncoder::Encode(const int16_t* pcm, std::size_t samples_per_channel,
                           std::vector<uint8_t>& out) {
  out.clear();
  if (encoder_ == nullptr) return AvpErr::kErrNotInit;
  if (pcm == nullptr) return AvpErr::kErrInvalidArg;
  if (samples_per_channel != frame_samples_) {
    dropped_samples_ += static_cast<uint32_t>(samples_per_channel);
    return AvpErr::kErrInvalidArg;
  }

  out.resize(kMaxPacketBytes);
  const int n = opus_encode(static_cast<::OpusEncoder*>(encoder_), pcm,
                            static_cast<int>(frame_samples_), out.data(),
                            static_cast<opus_int32>(out.size()));
  if (n < 0) {
    out.clear();
    return AvpErr::kErrFail;
  }
  out.resize(static_cast<std::size_t>(n));
  return AvpErr::kOk;
}

}  // namespace recorder

#endif  // RECORDER_MCU_LN882H
