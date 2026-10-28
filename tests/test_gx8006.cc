#include "cl_test.h"

#include <cstring>
#include <vector>

#include "cl_gx8006_audio_source.h"
#include "cl_gx8006_protocol.h"
#include "cl_ln882h_capabilities.h"

namespace {

std::vector<uint8_t> GxFrame(uint8_t subcmd, const std::vector<uint8_t>& body,
                             bool response = true) {
  std::vector<uint8_t> payload;
  payload.push_back(subcmd);
  payload.insert(payload.end(), body.begin(), body.end());
  std::vector<uint8_t> f;
  f.push_back(0x55);
  f.push_back(0xAA);
  f.push_back(response ? 0x03 : 0x00);
  f.push_back(0x92);
  f.push_back(static_cast<uint8_t>(payload.size() >> 8));
  f.push_back(static_cast<uint8_t>(payload.size()));
  f.insert(f.end(), payload.begin(), payload.end());
  uint8_t sum = 0;
  for (uint8_t b : f) sum += b;
  f.push_back(sum);
  return f;
}

std::vector<uint8_t> VersionResponse(uint8_t query_type) {
  return GxFrame(0x01, {query_type, 0x00, 0x00, '1', '.', '0'});
}

std::vector<uint8_t> MicPacket(const std::vector<uint8_t>& audio) {
  std::vector<uint8_t> body;
  body.push_back(1);  // MID
  body.insert(body.end(), audio.begin(), audio.end());
  return GxFrame(0x05, body);
}

std::vector<uint8_t> MicMarker(uint8_t indicator) { return GxFrame(0x05, {indicator}); }

struct FakeUart {
  recorder::Gx8006Protocol* proto = nullptr;
  std::vector<std::vector<uint8_t>> writes;
  bool auto_version = true;

  int Write(uint8_t* data, std::size_t len) {
    writes.emplace_back(data, data + len);
    if (len >= 7 && auto_version) {
      const uint8_t subcmd = data[6];
      if (subcmd == 0x01) {
        const auto resp = VersionResponse(1);
        proto->Input(resp.data(), resp.size());
      }
    }
    return static_cast<int>(len);
  }
};

uint8_t request_subcmd(const std::vector<uint8_t>& frame) {
  return frame.size() > 6 ? frame[6] : 0;
}

uint32_t read_u32(const std::vector<uint8_t>& f, std::size_t off) {
  return (static_cast<uint32_t>(f[off]) << 24) | (static_cast<uint32_t>(f[off + 1]) << 16) |
         (static_cast<uint32_t>(f[off + 2]) << 8) | static_cast<uint32_t>(f[off + 3]);
}

recorder::AudioConfig OpusConfig() {
  recorder::AudioConfig c;
  c.format = recorder::AudioFormat::kOpus;
  c.sample_rate = 16000;
  c.channels = 1;
  c.frame_ms = 20;
  c.opus_encoder = "gx8006";
  return c;
}

}  // namespace

TEST(gx_handshake_sends_expected_configuration) {
  recorder::Gx8006Protocol proto;
  FakeUart uart;
  uart.proto = &proto;
  proto.Init([&](const uint8_t* d, std::size_t n) { return uart.Write(const_cast<uint8_t*>(d), n); });

  recorder::GxMicConfig cfg;
  cfg.gain_db = 24;
  ASSERT_TRUE(proto.Handshake(cfg) == recorder::AvpErr::kOk);
  EXPECT_TRUE(proto.ready());
  ASSERT_TRUE(uart.writes.size() >= 6);

  bool saw_version = false, saw_mic = false, saw_spk = false, saw_pickup = false;
  bool saw_vad_off = false, saw_mic_on = false;
  for (const auto& w : uart.writes) {
    switch (request_subcmd(w)) {
      case 0x01:
        saw_version = true;
        break;
      case 0x02:
        saw_mic = true;
        EXPECT_EQ(read_u32(w, 7), 16000u);
        EXPECT_EQ(w[11], 16);  // bits
        EXPECT_EQ(w[12], 1);   // channels
        EXPECT_EQ(w[13], 24);  // gain
        EXPECT_EQ(w[14], 2);   // Opus
        EXPECT_EQ(read_u32(w, 15), 761u);
        break;
      case 0x03:
        saw_spk = true;
        EXPECT_EQ(w[13], 2);  // Opus speaker format
        break;
      case 0x07:
        if (w[7] == 0x08) {
          saw_pickup = true;
          EXPECT_EQ(w[8], 60);
        } else if (w[7] == 0x04) {
          saw_vad_off = true;
          EXPECT_EQ(w[8], 0);
        } else if (w[7] == 0x01) {
          saw_mic_on = true;
          EXPECT_EQ(w[8], 1);
        }
        break;
      default:
        break;
    }
  }
  EXPECT_TRUE(saw_version);
  EXPECT_TRUE(saw_mic);
  EXPECT_TRUE(saw_spk);
  EXPECT_TRUE(saw_pickup);
  EXPECT_TRUE(saw_vad_off);
  EXPECT_TRUE(saw_mic_on);
}

TEST(gx_source_preserves_opus_packets_and_timestamps) {
  recorder::Gx8006Protocol proto;
  FakeUart uart;
  uart.proto = &proto;
  proto.Init([&](const uint8_t* d, std::size_t n) { return uart.Write(const_cast<uint8_t*>(d), n); });
  recorder::Gx8006AudioSource src(&proto);
  ASSERT_TRUE(src.Configure(OpusConfig()) == recorder::AvpErr::kOk);
  ASSERT_TRUE(src.Start() == recorder::AvpErr::kOk);

  std::vector<uint8_t> packet(74, 0x11);
  packet[0] = 0x48;  // SILK WB 20 ms
  packet[73] = 0x22;  // odd-ish tail; length stays exact
  auto start = MicMarker(0);
  auto mid = MicPacket(packet);
  auto stop = MicMarker(2);
  proto.Input(start.data(), start.size());
  proto.Input(mid.data(), mid.size());
  proto.Input(stop.data(), stop.size());
  proto.Process();

  recorder::CaptureFrame f;
  ASSERT_TRUE(src.Read(f, 0) == recorder::AvpErr::kOk);
  EXPECT_EQ(f.bytes, 74u);
  EXPECT_EQ(f.samples, 320u);
  EXPECT_EQ(f.timestamp_ms, 0u);
  EXPECT_TRUE(f.format == recorder::AudioFormat::kOpus);
  EXPECT_TRUE(f.pcm == nullptr);
  EXPECT_FALSE(f.stats_valid);
  EXPECT_EQ(f.data[0], 0x48);
  EXPECT_EQ(f.data[73], 0x22);

  // A second 20 ms packet advances the audio clock by 20 ms even though it is
  // an odd length (75 bytes).
  std::vector<uint8_t> odd(75, 0x33);
  odd[0] = 0x48;
  auto mid2 = MicPacket(odd);
  proto.Input(mid2.data(), mid2.size());
  proto.Process();
  ASSERT_TRUE(src.Read(f, 0) == recorder::AvpErr::kOk);
  EXPECT_EQ(f.bytes, 75u);
  EXPECT_EQ(f.samples, 320u);
  EXPECT_EQ(f.timestamp_ms, 20u);
  EXPECT_EQ(src.CountersSnapshot().truncations, 0u);
}

TEST(gx_source_marks_queue_overflow_discontinuity) {
  recorder::Gx8006Protocol proto;
  FakeUart uart;
  uart.proto = &proto;
  proto.Init([&](const uint8_t* d, std::size_t n) { return uart.Write(const_cast<uint8_t*>(d), n); });
  recorder::Gx8006AudioSource src(&proto, 2);
  ASSERT_TRUE(src.Configure(OpusConfig()) == recorder::AvpErr::kOk);
  ASSERT_TRUE(src.Start() == recorder::AvpErr::kOk);

  const auto start = MicMarker(0);
  proto.Input(start.data(), start.size());
  for (int i = 0; i < 3; ++i) {
    std::vector<uint8_t> audio(74, static_cast<uint8_t>(i));
    audio[0] = 0x48;
    auto f = MicPacket(audio);
    proto.Input(f.data(), f.size());
  }
  proto.Process();
  EXPECT_EQ(src.CountersSnapshot().queue_drops, 1u);

  recorder::CaptureFrame f;
  ASSERT_TRUE(src.Read(f, 0) == recorder::AvpErr::kOk);
  EXPECT_TRUE(f.discontinuity);
  EXPECT_EQ(f.data[1], 1u);  // oldest (0) was dropped, 1 is next
}

TEST(gx_source_rehandshakes_after_chip_restart) {
  recorder::Gx8006Protocol proto;
  FakeUart uart;
  uart.proto = &proto;
  proto.Init([&](const uint8_t* d, std::size_t n) { return uart.Write(const_cast<uint8_t*>(d), n); });
  recorder::Gx8006AudioSource src(&proto);
  ASSERT_TRUE(src.Configure(OpusConfig()) == recorder::AvpErr::kOk);
  ASSERT_TRUE(src.Start() == recorder::AvpErr::kOk);

  const auto restart = VersionResponse(2);
  proto.Input(restart.data(), restart.size());
  proto.Process();
  EXPECT_TRUE(proto.TakeChipRestart());

  recorder::CaptureFrame f;
  EXPECT_TRUE(src.Read(f, 0) == recorder::AvpErr::kErrTimeout);
  EXPECT_EQ(src.CountersSnapshot().chip_restarts, 1u);
  EXPECT_EQ(src.CountersSnapshot().rehandshakes, 1u);
}

TEST(gx_source_rejects_invalid_toc) {
  recorder::Gx8006Protocol proto;
  FakeUart uart;
  uart.proto = &proto;
  proto.Init([&](const uint8_t* d, std::size_t n) { return uart.Write(const_cast<uint8_t*>(d), n); });
  recorder::Gx8006AudioSource src(&proto);
  ASSERT_TRUE(src.Configure(OpusConfig()) == recorder::AvpErr::kOk);
  ASSERT_TRUE(src.Start() == recorder::AvpErr::kOk);

  const std::vector<uint8_t> bad = {0x03, 0x00};  // code 3 with zero frames
  auto f = MicPacket(bad);
  proto.Input(f.data(), f.size());
  proto.Process();
  recorder::CaptureFrame out;
  EXPECT_TRUE(src.Read(out, 0) == recorder::AvpErr::kErrTimeout);
  EXPECT_EQ(src.CountersSnapshot().toc_errors, 1u);
}

TEST(gx_capabilities_are_opus_only_and_honest) {
  const auto caps = recorder::BuildLn882hCapabilities();
  EXPECT_TRUE(caps.opus);
  EXPECT_FALSE(caps.pcm_s16le);
  EXPECT_FALSE(caps.test_tone);
  EXPECT_FALSE(caps.speaker);
  EXPECT_FALSE(caps.loopback);
  EXPECT_EQ(caps.device_pre_roll_max_ms, 0);
  EXPECT_TRUE(caps.server_pre_roll);
  EXPECT_TRUE(caps.gain_supported);
  EXPECT_EQ(caps.gain_min, 0);
  EXPECT_EQ(caps.gain_max, 42);
  EXPECT_EQ(caps.gain_step, 6);
  EXPECT_EQ(caps.max_frame_bytes, 2048);
  ASSERT_TRUE(caps.opus_cap.encoders.size() == 1);
  EXPECT_EQ(caps.opus_cap.encoders[0], std::string("gx8006"));
  EXPECT_TRUE(caps.opus_cap.payload_max == 255);
}
