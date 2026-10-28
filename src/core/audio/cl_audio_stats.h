#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cl_recorder_types.h"

namespace recorder {

// Aggregate PCM levels over all channels, normalized to S16 full scale.
struct PcmLevels {
  bool valid = false;
  double rms_dbfs = -200.0;
  double peak_dbfs = -200.0;
  double peak_linear = 0.0;   // 0..1
  double dc_offset_lsb = 0.0; // mean sample, -32768..32767
  std::size_t clipped_samples = 0;
  std::size_t total_samples = 0;
  FrameStats stats;
};

PcmLevels ComputePcmLevels(const int16_t* pcm, std::size_t samples, int channels);

double LinearToDbfs(double linear);

// Adaptive energy VAD used by the segmenter. Pure logic: the caller supplies
// the clock (frame duration) and the noise-floor update decision.
class EnergyVad {
 public:
  void Configure(double threshold_db, int calibration_frames = 5);
  void Reset();

  // Decide speech for this frame and update the noise floor when the frame is
  // not speech. Must be called exactly once per frame, in order.
  bool Analyze(double rms_dbfs);
  uint8_t Score(double rms_dbfs) const;

  double noise_dbfs() const { return noise_dbfs_; }
  bool calibrated() const { return frames_seen_ >= calibration_frames_; }

 private:
  double threshold_db_ = 6.0;
  int calibration_frames_ = 5;
  int frames_seen_ = 0;
  double noise_dbfs_ = -90.0;
  bool speech_ = false;
};

// 10th percentile; returns -200 for an empty list.
double Percentile10(std::vector<double> values);

}  // namespace recorder
