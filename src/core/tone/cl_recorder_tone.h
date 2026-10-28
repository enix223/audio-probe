#pragma once

#include <cstddef>
#include <cstdint>

#include "cl_recorder_types.h"

namespace recorder {

// Deterministic sine/square/sweep generator with fade-in/out. The caller owns
// the speaker sink and the pacing; this class only renders PCM.
class ToneGenerator {
 public:
  AvpErr Init(const ToneMetadata& tone, int sample_rate, int channels);
  void Reset();

  // Renders up to `frames` per-channel frames interleaved into out. Returns
  // the number of frames written; done() becomes true at the end of the tone.
  std::size_t Render(int16_t* out, std::size_t frames);

  bool done() const { return frames_emitted_ >= total_frames_; }
  std::size_t total_frames() const { return total_frames_; }
  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }

 private:
  double FrequencyAt(std::size_t frame) const;
  double EnvelopeAt(std::size_t frame) const;

  ToneMetadata tone_;
  int sample_rate_ = 16000;
  int channels_ = 1;
  std::size_t total_frames_ = 0;
  std::size_t frames_emitted_ = 0;
  std::size_t fade_frames_ = 0;
  double phase_ = 0.0;
  double amplitude_ = 0.0;
  bool sweep_ = false;
  bool log_sweep_ = false;
};

}  // namespace recorder
