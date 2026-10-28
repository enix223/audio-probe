#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cl_audio_source.h"
#include "cl_gx8006_protocol.h"
#include "cl_recorder_mutex.h"

namespace recorder {

// Byte-exact GX8006 microphone source. The first milestone is chip-owned Opus
// (PCM is not advertised until a bench test proves GET_MIC_DATA carries PCM).
//
// The chip RX task feeds Gx8006Protocol::Input(); the protocol callback copies
// each packet into a fixed-slot queue; Read() pops one complete packet. Odd
// Opus lengths are preserved and never reinterpreted as int16_t.
class Gx8006AudioSource : public AudioSource {
 public:
  static constexpr std::size_t kPacketMax = 320;  // Opus payload <= 255 + margin

  struct Counters {
    uint32_t packets = 0;
    uint32_t frames_read = 0;
    uint32_t queue_drops = 0;
    uint32_t truncations = 0;
    uint32_t toc_errors = 0;
    uint32_t uart_overruns = 0;
    uint32_t chip_restarts = 0;
    uint32_t rehandshakes = 0;
    uint32_t mic_starts = 0;
    uint32_t mic_stops = 0;
  };

  explicit Gx8006AudioSource(Gx8006Protocol* protocol, std::size_t queue_slots = 16);
  ~Gx8006AudioSource() override = default;

  AvpErr Configure(const AudioConfig& cfg) override;
  AvpErr Start() override;
  void Stop() override;

  AudioFormat NativeFormat() const override;
  bool ProvidesPcm() const override { return false; }

  AvpErr Read(CaptureFrame& out, uint32_t timeout_ms) override;
  SourceStats Stats() const override;
  AvpErr SetGain(int db) override;

  Counters CountersSnapshot() const;

 private:
  struct Packet {
    uint16_t len = 0;
    bool discontinuity = false;
    uint8_t data[kPacketMax] = {};
  };

  void OnMicData(const uint8_t* data, std::size_t len);
  void OnMicStart();
  void OnMicStop();
  void OnChipRestart();
  bool Pop(Packet* out);

  Gx8006Protocol* proto_;
  AudioConfig cfg_;
  GxMicConfig mic_cfg_;
  int gain_db_ = 26;
  bool started_ = false;
  std::atomic<bool> restart_pending_{false};
  bool disc_next_ = false;
  uint64_t samples_emitted_ = 0;
  Packet current_;

  mutable RecorderMutex mu_;
  std::vector<Packet> ring_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::size_t count_ = 0;
  bool pending_disc_ = false;
  Counters counters_;
};

}  // namespace recorder
