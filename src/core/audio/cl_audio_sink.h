#pragma once

#include <cstddef>
#include <cstdint>

#include "cl_recorder_types.h"

namespace recorder {

// Speaker/tone sink abstraction. The test-tone generator is core code; the
// platform supplies a sink (Linux: miniaudio playback).
class AudioSink {
 public:
  virtual ~AudioSink() = default;
  virtual AvpErr Init(int sample_rate, int channels) = 0;
  virtual void Deinit() = 0;
  virtual bool ready() const = 0;
  virtual AvpErr Write(const int16_t* pcm, std::size_t frames, int timeout_ms) = 0;
  virtual void SetVolume(int percent) = 0;
};

}  // namespace recorder
