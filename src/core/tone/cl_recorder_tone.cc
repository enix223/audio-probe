#include "cl_recorder_tone.h"

#include <algorithm>
#include <cmath>

namespace recorder {

namespace {
constexpr double kPi = 3.14159265358979323846;

int16_t ToS16(double v) {
  const double clipped = std::max(-1.0, std::min(1.0, v));
  return static_cast<int16_t>(std::lround(clipped * 32767.0));
}
}  // namespace

AvpErr ToneGenerator::Init(const ToneMetadata& tone, int sample_rate, int channels) {
  if (sample_rate <= 0 || channels <= 0) return AvpErr::kErrInvalidArg;
  tone_ = tone;
  sample_rate_ = sample_rate;
  channels_ = channels;
  phase_ = 0.0;

  const int duration_ms = tone.duration_ms > 0 ? tone.duration_ms : 5000;
  total_frames_ = static_cast<std::size_t>(duration_ms) *
                  static_cast<std::size_t>(sample_rate) / 1000;
  frames_emitted_ = 0;
  fade_frames_ = static_cast<std::size_t>(std::max(0, tone.fade_ms)) *
                 static_cast<std::size_t>(sample_rate) / 1000;
  amplitude_ = std::pow(10.0, std::max(-120.0, std::min(0.0, tone.amplitude_dbfs)) / 20.0);

  sweep_ = tone.waveform == "sweep";
  log_sweep_ = sweep_ && tone.sweep_mode == "log";
  if (sweep_) {
    if (tone.start_hz <= 0.0 || tone.end_hz <= 0.0) return AvpErr::kErrInvalidArg;
  } else if (tone.frequency_hz <= 0.0) {
    return AvpErr::kErrInvalidArg;
  }
  return AvpErr::kOk;
}

void ToneGenerator::Reset() {
  frames_emitted_ = 0;
  phase_ = 0.0;
}

double ToneGenerator::FrequencyAt(std::size_t frame) const {
  if (!sweep_ || total_frames_ <= 1) {
    return sweep_ ? tone_.start_hz : tone_.frequency_hz;
  }
  const double t = static_cast<double>(frame) / static_cast<double>(total_frames_ - 1);
  if (log_sweep_) {
    return tone_.start_hz * std::pow(tone_.end_hz / tone_.start_hz, t);
  }
  return tone_.start_hz + (tone_.end_hz - tone_.start_hz) * t;
}

double ToneGenerator::EnvelopeAt(std::size_t frame) const {
  if (fade_frames_ == 0) return 1.0;
  const double in = std::min(1.0, static_cast<double>(frame) / static_cast<double>(fade_frames_));
  const std::size_t remaining = total_frames_ > frame ? total_frames_ - frame : 0;
  const double out = std::min(1.0, static_cast<double>(remaining) / static_cast<double>(fade_frames_));
  return std::min(in, out);
}

std::size_t ToneGenerator::Render(int16_t* out, std::size_t frames) {
  if (out == nullptr || frames == 0) return 0;
  if (frames_emitted_ >= total_frames_) return 0;

  const std::size_t n = std::min(frames, total_frames_ - frames_emitted_);
  const bool square = tone_.waveform == "square";
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t frame = frames_emitted_ + i;
    const double env = EnvelopeAt(frame);
    double s;
    if (square) {
      s = (phase_ < 0.5) ? amplitude_ : -amplitude_;
    } else {
      s = std::sin(2.0 * kPi * phase_) * amplitude_;
    }
    s *= env;
    const int16_t v = ToS16(s);
    for (int c = 0; c < channels_; ++c) out[i * channels_ + c] = v;

    const double freq = FrequencyAt(frame);
    phase_ += freq / static_cast<double>(sample_rate_);
    phase_ -= std::floor(phase_);
  }
  frames_emitted_ += n;
  return n;
}

}  // namespace recorder
