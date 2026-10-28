#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cl_recorder_types.h"

namespace recorder {

// Host Opus encoder (libopus). The recorder owns this wrapper rather than
// depending on ai-voice; the wire requirements are the same one-packet-per-
// frame contract as docs/protocol.md §6.
class OpusEncoder {
 public:
  OpusEncoder() = default;
  ~OpusEncoder();
  OpusEncoder(const OpusEncoder&) = delete;
  OpusEncoder& operator=(const OpusEncoder&) = delete;

  AvpErr Init(const AudioConfig& cfg);
  void Reset();

  std::size_t FrameSamples() const { return frame_samples_; }
  bool ready() const { return encoder_ != nullptr; }

  // Encodes exactly one frame. `samples_per_channel` must equal
  // FrameSamples(); a different count is rejected rather than silently
  // truncated. `out` receives one complete Opus packet.
  AvpErr Encode(const int16_t* pcm, std::size_t samples_per_channel,
                std::vector<uint8_t>& out);

  uint32_t dropped_samples() const { return dropped_samples_; }

 private:
  void* encoder_ = nullptr;  // ::OpusEncoder*
  AudioConfig cfg_;
  std::size_t frame_samples_ = 0;
  uint32_t dropped_samples_ = 0;
};

}  // namespace recorder
