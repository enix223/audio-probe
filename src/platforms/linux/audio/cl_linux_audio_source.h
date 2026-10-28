#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cl_audio_source.h"

namespace recorder {

// miniaudio microphone capture. The audio callback only copies S16 samples
// into a lock-free ring; framing, statistics, VAD, and encoding all happen on
// the app task.
class LinuxAudioSource : public AudioSource {
 public:
  LinuxAudioSource() = default;
  ~LinuxAudioSource() override;

  AvpErr Configure(const AudioConfig& cfg) override;
  AvpErr Start() override;
  void Stop() override;

  AudioFormat NativeFormat() const override { return AudioFormat::kPcmS16le; }
  bool ProvidesPcm() const override { return true; }

  AvpErr Read(CaptureFrame& out, uint32_t timeout_ms) override;
  SourceStats Stats() const override;

  // Called from the miniaudio capture callback (real-time thread).
  void OnCapture(const int16_t* data, std::size_t samples);

 private:
  class Ring;

  void DestroyDevice();

  void* device_ = nullptr;  // ma_device*
  Ring* ring_ = nullptr;
  AudioConfig cfg_;
  bool started_ = false;

  std::vector<int16_t> accum_;
  std::size_t accum_pos_ = 0;
  std::vector<int16_t> frame_buf_;
  std::size_t frame_samples_ = 0;
  std::size_t frame_values_ = 0;

  uint64_t frame_count_ = 0;
  uint64_t emitted_samples_ = 0;
  mutable uint64_t overruns_ = 0;
  uint64_t last_ring_dropped_ = 0;
  mutable uint64_t errors_ = 0;
};

}  // namespace recorder
