#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "cl_recorder_mutex.h"
#include "cl_recorder_types.h"

namespace recorder {

// GX8006 SmartBot framing (UART, 1 Mbps, 8N1).
//
//   wire: 55 AA | version | cmd | len_hi len_lo | payload | checksum
//   version 0x00 = request, 0x03 = response; cmd 0x92; checksum = sum & 0xFF.
//
// This is a recorder-specific implementation. It deliberately does not depend
// on ai-voice's voice-coupled HalAudioGx8006. The chip owns the mic/speaker
// path; KWS is never armed. Capture is started by MQTT.

constexpr uint8_t kGxHead0 = 0x55;
constexpr uint8_t kGxHead1 = 0xAA;
constexpr uint8_t kGxVersionRequest = 0x00;
constexpr uint8_t kGxVersionResponse = 0x03;
constexpr uint8_t kGxCmd = 0x92;
constexpr std::size_t kGxHeaderSize = 6;
constexpr std::size_t kGxMaxRxFrame = 768;       // payload + header + checksum
constexpr std::size_t kGxMaxRxPayload = kGxMaxRxFrame - kGxHeaderSize - 1;
constexpr std::size_t kGxMaxTxPayload = 300;
constexpr std::size_t kGxRxRingSize = 4096;      // power of two
constexpr std::size_t kGxMaxSpkData = 255;

enum class GxSubCmd : uint8_t {
  kVersionQuery = 0x01,
  kSetMicConfig = 0x02,
  kSetSpkConfig = 0x03,
  kSetWakeupPin = 0x04,
  kGetMicData = 0x05,
  kSetSpkData = 0x06,
  kSetOfflineConfig = 0x07,
  kGetOfflineWakeup = 0x08,
  kGetTimeout = 0x09,
  kEventNotify = 0x0F,
  kEventNotifyV2 = 0x12,
};

enum class GxOfflineSubCmd : uint8_t {
  kMic = 0x01,
  kWakeup = 0x03,
  kVad = 0x04,
  kWakeNotify = 0x05,
  kResetTimeout = 0x06,
  kSetKwsTimeout = 0x07,
  kSetPickupPeriod = 0x08,
  kVadSensitivity = 0x09,
};

enum class GxIndicator : uint8_t { kStart = 0, kMid = 1, kStop = 2 };
enum class GxAudioFormat : uint8_t { kPcm = 0, kSpeex = 1, kOpus = 2, kMp3 = 3 };

struct GxMicConfig {
  int sample_rate = 16000;
  int bits = 16;
  int channels = 1;
  int gain_db = 26;
  bool opus = true;  // first milestone: chip-owned Opus only
  int max_recv_bytes = static_cast<int>(kGxMaxRxPayload);
};

struct GxProtocolStats {
  uint32_t rx_bytes = 0;
  uint32_t frames = 0;
  uint32_t parse_errors = 0;
  uint32_t checksum_errors = 0;
  uint32_t rx_overflow = 0;
  uint32_t mic_frames = 0;
  uint32_t mic_starts = 0;
  uint32_t mic_stops = 0;
  uint32_t chip_restarts = 0;
  uint32_t unknown_frames = 0;
  uint32_t wake_events = 0;
};

class Gx8006Protocol {
 public:
  using WriteFn = std::function<int(const uint8_t* data, std::size_t len)>;
  using FcReadyFn = std::function<bool()>;
  using DelayFn = std::function<void(uint32_t ms)>;
  using MicDataFn = std::function<void(const uint8_t* data, std::size_t len)>;
  using EventFn = std::function<void()>;
  using TimeoutFn = std::function<void()>;

  Gx8006Protocol() = default;

  void Init(WriteFn write, FcReadyFn fc_ready = nullptr, DelayFn delay = nullptr);
  bool ready() const { return ready_; }

  // Handshake, configure the mic and speaker, disable VAD, enable continuous
  // mic upload. Blocks up to ~1 s while polling the RX parser.
  AvpErr Handshake(const GxMicConfig& cfg);

  AvpErr SendVersionQuery();
  AvpErr SetMicrophone(bool on);
  AvpErr SetVad(uint8_t value);  // 0 = off (continuous upload)
  AvpErr SetPickupPeriod(uint8_t seconds);
  AvpErr SetMicGain(int db);
  AvpErr ReArm();  // pickup period + VAD off + mic on (no KWS)

  // Speaker path (test tone). Not advertised until bench-validated.
  AvpErr SendSpeakerStart();
  AvpErr SendSpeakerData(const uint8_t* data, std::size_t len);
  AvpErr SendSpeakerStop();
  bool WaitFlowControl(uint32_t timeout_ms);

  // UART RX task input; Process() parses buffered frames. Both are safe to
  // call from different tasks (the internal mutex serializes them).
  void Input(const uint8_t* data, std::size_t len);
  void Process();
  void Drain();

  // Returns true once after a chip restart/reboot response was observed.
  bool TakeChipRestart();

  void SetOnMicData(MicDataFn cb) { on_mic_data_ = std::move(cb); }
  void SetOnMicStart(EventFn cb) { on_mic_start_ = std::move(cb); }
  void SetOnMicStop(EventFn cb) { on_mic_stop_ = std::move(cb); }
  void SetOnChipRestart(EventFn cb) { on_chip_restart_ = std::move(cb); }
  void SetOnSessionTimeout(TimeoutFn cb) { on_session_timeout_ = std::move(cb); }

  GxProtocolStats Stats() const;

 private:
  AvpErr SendFrame(const uint8_t* payload, std::size_t len);
  AvpErr SendMicConfig();
  AvpErr SendSpkConfig();
  AvpErr SendOffline(GxOfflineSubCmd sub, uint8_t value);
  bool WaitFlag(const std::atomic<bool>& flag, uint32_t timeout_ms);
  void Dispatch(const uint8_t* payload, std::size_t len);

  std::size_t Avail() const;
  uint8_t Peek(std::size_t offset) const;
  void Drop(std::size_t n);
  void CopyOut(std::size_t offset, uint8_t* dst, std::size_t n) const;

  WriteFn write_;
  FcReadyFn fc_ready_;
  DelayFn delay_;
  bool ready_ = false;
  std::atomic<bool> version_seen_{false};
  std::atomic<bool> chip_restart_{false};

  GxMicConfig cfg_;
  uint32_t spk_pkt_ = 0;
  int volume_ = 60;

  mutable RecorderMutex mu_;
  uint8_t rx_[kGxRxRingSize] = {};
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  GxProtocolStats stats_;

  MicDataFn on_mic_data_;
  EventFn on_mic_start_;
  EventFn on_mic_stop_;
  EventFn on_chip_restart_;
  TimeoutFn on_session_timeout_;
};

}  // namespace recorder
