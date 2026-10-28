#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cl_audio_sink.h"
#include "cl_recorder_types.h"

namespace recorder {

// miniaudio playback sink used by the test-tone generator. The playback
// callback only drains a lock-free ring; the app task writes complete chunks.
class LinuxAudioSink : public AudioSink {
 public:
  LinuxAudioSink() = default;
  ~LinuxAudioSink() override;

  AvpErr Init(int sample_rate, int channels) override;
  void Deinit() override;
  bool ready() const override { return device_ != nullptr; }

  AvpErr Write(const int16_t* pcm, std::size_t frames, int timeout_ms) override;
  void SetVolume(int percent) override;

  uint64_t dropped_samples() const;

  // Called from the miniaudio playback callback (real-time thread).
  void OnPlayback(int16_t* output, std::size_t samples);

 private:
  class Ring;

  void* device_ = nullptr;  // ma_device*
  Ring* ring_ = nullptr;
  int sample_rate_ = 16000;
  int channels_ = 1;
};

}  // namespace recorder
