#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "cl_audio_source.h"

namespace recorder {

// Deterministic PCM replay source for host tests and end-to-end runs without
// a microphone. Supports raw S16LE and WAV (PCM S16LE).
class FileAudioSource : public AudioSource {
 public:
  FileAudioSource(std::string path, bool realtime, bool loop);

  AvpErr Configure(const AudioConfig& cfg) override;
  AvpErr Start() override;
  void Stop() override;

  AudioFormat NativeFormat() const override { return AudioFormat::kPcmS16le; }
  bool ProvidesPcm() const override { return true; }

  AvpErr Read(CaptureFrame& out, uint32_t timeout_ms) override;
  SourceStats Stats() const override;

 private:
  AvpErr ParseWav();
  bool FillFrame();

  std::string path_;
  bool realtime_ = true;
  bool loop_ = true;
  std::ifstream file_;
  bool wav_ = false;
  std::size_t data_offset_ = 0;
  std::size_t data_bytes_ = 0;
  int file_rate_ = 0;
  int file_channels_ = 0;
  int file_bits_ = 0;

  AudioConfig cfg_;
  std::size_t frame_samples_ = 0;
  std::size_t frame_values_ = 0;
  std::vector<int16_t> frame_buf_;
  uint64_t samples_emitted_ = 0;
  uint64_t frames_emitted_ = 0;
  bool eof_ = false;
  uint64_t errors_ = 0;

  std::chrono::steady_clock::time_point next_due_{};
  bool paced_ = false;
};

}  // namespace recorder
