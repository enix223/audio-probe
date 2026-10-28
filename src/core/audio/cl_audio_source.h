#pragma once

#include <cstddef>
#include <cstdint>

#include "cl_recorder_types.h"

namespace recorder {

// Capture abstraction. Implementations return byte-exact frames; Read() must
// never truncate. `data` points into source-owned storage valid until the next
// Read() call.
class AudioSource {
 public:
  virtual ~AudioSource() = default;

  // Configure for the negotiated rate/channels/frame duration. The source may
  // still produce its native format (Linux: PCM even when host Opus was
  // negotiated); NativeFormat() tells the pipeline what to expect.
  virtual AvpErr Configure(const AudioConfig& cfg) = 0;
  virtual AvpErr Start() = 0;
  virtual void Stop() = 0;

  virtual AudioFormat NativeFormat() const = 0;
  virtual bool ProvidesPcm() const = 0;

  // Non-blocking when timeout_ms == 0: returns kErrTimeout when no complete
  // frame is available yet.
  virtual AvpErr Read(CaptureFrame& out, uint32_t timeout_ms) = 0;

  virtual SourceStats Stats() const = 0;

  // Optional hardware gain. Linux returns kErrNotSupported.
  virtual AvpErr SetGain(int /*db*/) { return AvpErr::kErrNotSupported; }
};

}  // namespace recorder
