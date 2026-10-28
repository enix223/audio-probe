#include "cl_audio_stats.h"

#include <algorithm>
#include <cmath>

namespace recorder {

namespace {
constexpr double kEpsilon = 1e-12;
constexpr int kClipThreshold = 32760;
}  // namespace

double LinearToDbfs(double linear) { return 20.0 * std::log10(linear + kEpsilon); }

PcmLevels ComputePcmLevels(const int16_t* pcm, std::size_t samples, int channels) {
  PcmLevels out;
  if (pcm == nullptr || samples == 0 || channels <= 0) return out;

  const std::size_t n = samples * static_cast<std::size_t>(channels);
  double sum = 0.0;
  double sum_sq = 0.0;
  int peak_abs = 0;
  std::size_t clipped = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const int v = pcm[i];
    const int a = v < 0 ? -v : v;
    if (a > peak_abs) peak_abs = a;
    if (a >= kClipThreshold) ++clipped;
    sum += v;
    sum_sq += static_cast<double>(v) * static_cast<double>(v);
  }
  const double rms_linear = std::sqrt(sum_sq / static_cast<double>(n)) / 32768.0;
  const double peak_linear = static_cast<double>(peak_abs) / 32768.0;
  const double dc = sum / static_cast<double>(n);

  out.valid = true;
  out.rms_dbfs = LinearToDbfs(rms_linear);
  out.peak_dbfs = LinearToDbfs(peak_linear);
  out.peak_linear = peak_linear;
  out.dc_offset_lsb = dc;
  out.clipped_samples = clipped;
  out.total_samples = n;

  // Header fields are in fixed units; clamp rather than overflow.
  const double rms_x100 = out.rms_dbfs * 100.0;
  out.stats.valid = true;
  out.stats.rms_dbfs = out.rms_dbfs;
  out.stats.rms_dbfs_x100 = static_cast<int16_t>(
      std::max(-32768.0, std::min(32767.0, std::round(rms_x100))));
  out.stats.peak = static_cast<uint16_t>(std::min(65534, peak_abs));
  out.stats.dc_offset = static_cast<int16_t>(
      std::max(-32767.0, std::min(32767.0, std::round(dc))));
  out.stats.clipped = clipped > 0;
  return out;
}

void EnergyVad::Configure(double threshold_db, int calibration_frames) {
  threshold_db_ = threshold_db;
  calibration_frames_ = calibration_frames > 0 ? calibration_frames : 1;
  Reset();
}

void EnergyVad::Reset() {
  frames_seen_ = 0;
  noise_dbfs_ = -90.0;
  speech_ = false;
}

bool EnergyVad::Analyze(double rms_dbfs) {
  if (!std::isfinite(rms_dbfs) || rms_dbfs < -200.0) rms_dbfs = -200.0;
  if (rms_dbfs > 0.0) rms_dbfs = 0.0;

  ++frames_seen_;
  if (frames_seen_ <= calibration_frames_) {
    // Startup: the first frames define the room floor. Never report speech
    // from a floor that has not been established yet.
    noise_dbfs_ = noise_dbfs_ < -150.0 ? rms_dbfs : (0.7 * noise_dbfs_ + 0.3 * rms_dbfs);
    speech_ = false;
    return false;
  }

  speech_ = rms_dbfs > noise_dbfs_ + threshold_db_;

  // Track the floor on non-speech frames only. Adapt down quickly (a quieter
  // room) and up slowly (a fan switching on must not instantly swallow speech).
  if (!speech_) {
    const double alpha = rms_dbfs < noise_dbfs_ ? 0.08 : 0.01;
    noise_dbfs_ = (1.0 - alpha) * noise_dbfs_ + alpha * rms_dbfs;
    noise_dbfs_ = std::max(-200.0, std::min(0.0, noise_dbfs_));
  }
  return speech_;
}

uint8_t EnergyVad::Score(double rms_dbfs) const {
  if (!calibrated()) return 0;
  const double margin = rms_dbfs - noise_dbfs_;
  if (margin <= 0.0) return 0;
  const double score = margin / 40.0 * 100.0;
  return static_cast<uint8_t>(std::max(0.0, std::min(100.0, std::round(score))));
}

double Percentile10(std::vector<double> values) {
  if (values.empty()) return -200.0;
  std::sort(values.begin(), values.end());
  const std::size_t idx = values.size() / 10;  // floor(10%)
  return values[std::min(idx, values.size() - 1)];
}

}  // namespace recorder
