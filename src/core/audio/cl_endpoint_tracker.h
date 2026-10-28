#pragma once

#include <cstdint>

namespace recorder {

// Endpoint state machine driven by a per-frame speech/silence decision.
//
// Recorder-specific port of the proven ai-voice VadEndpointTracker semantics,
// without the guard window (the recorder segmenter applies its own guard so
// that pre-roll frames are still forwarded). Pure logic: time advances by the
// frame duration the caller supplies.
//
// A turn can only end once speech has been heard. That guard prevents an
// empty room from producing an endless stream of zero-length segments.
class EndpointTracker {
 public:
  struct Config {
    uint32_t onset_ms = 120;      // consecutive speech required to confirm onset
    uint32_t silence_ms = 700;    // trailing silence that ends the session
    uint32_t min_speech_ms = 150; // speech accumulated before a segment opens
  };

  EndpointTracker() = default;
  explicit EndpointTracker(const Config& cfg) : cfg_(cfg) {}

  void Configure(const Config& cfg) {
    cfg_ = cfg;
    Reset();
  }

  // Feed one frame's decision. Returns true exactly once per session, on the
  // frame where trailing silence first exceeds silence_ms.
  bool Feed(bool speech, uint32_t frame_ms) {
    if (speech) {
      if (speech_run_ms_ < cfg_.onset_ms) {
        speech_run_ms_ += frame_ms;
        if (speech_run_ms_ < cfg_.onset_ms) return false;
      }
      silence_run_ms_ = 0;
      if (!speech_seen_) {
        armed_ms_ += frame_ms;
        if (armed_ms_ >= cfg_.min_speech_ms) speech_seen_ = true;
      }
      return false;
    }

    speech_run_ms_ = 0;
    if (!speech_seen_) return false;

    silence_run_ms_ += frame_ms;
    if (silence_run_ms_ < cfg_.silence_ms) return false;

    speech_seen_ = false;
    armed_ms_ = 0;
    silence_run_ms_ = 0;
    return true;
  }

  void Reset() {
    speech_run_ms_ = 0;
    armed_ms_ = 0;
    silence_run_ms_ = 0;
    speech_seen_ = false;
  }

  bool SpeechSeen() const { return speech_seen_; }
  const Config& config() const { return cfg_; }

 private:
  Config cfg_{};
  uint32_t speech_run_ms_ = 0;
  uint32_t armed_ms_ = 0;
  uint32_t silence_run_ms_ = 0;
  bool speech_seen_ = false;
};

}  // namespace recorder
