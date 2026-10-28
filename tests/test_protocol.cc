#include "cl_test.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "cl_recorder_protocol.h"
#include "rapidjson/document.h"

namespace {

std::string GoldenPath(const char* name) {
  return std::string(RECORDER_ROOT) + "/protocol/golden/" + name;
}

std::vector<uint8_t> ReadBin(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

std::string ReadText(const std::string& path) {
  std::ifstream f(path);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(header_layout_is_28_bytes_big_endian) {
  recorder::AudioHeader h;
  h.version = 1;
  h.type = 1;
  h.flags = 0x21;
  h.format = recorder::AudioFormat::kPcmS16le;
  h.segment_id = 0x01020304;
  h.seq = 0x05060708;
  h.timestamp_ms = 0x090A0B0C;
  h.samples = 0x0D0E;
  h.payload_len = 0x0F10;
  h.rms_dbfs_x100 = -100;
  h.peak = 0x1234;
  h.dc_offset = -2;
  h.vad_score = 7;
  h.reserved = 0;

  uint8_t out[recorder::kAudioHeaderSize];
  recorder::EncodeAudioHeader(h, out);
  EXPECT_EQ(recorder::kAudioHeaderSize, 28u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 1);
  EXPECT_EQ(out[2], 0x21);
  EXPECT_EQ(out[3], 1);
  EXPECT_EQ(out[4], 0x01);
  EXPECT_EQ(out[5], 0x02);
  EXPECT_EQ(out[6], 0x03);
  EXPECT_EQ(out[7], 0x04);
  EXPECT_EQ(out[8], 0x05);
  EXPECT_EQ(out[11], 0x08);
  EXPECT_EQ(out[12], 0x09);
  EXPECT_EQ(out[15], 0x0C);
  EXPECT_EQ(out[16], 0x0D);
  EXPECT_EQ(out[17], 0x0E);
  EXPECT_EQ(out[18], 0x0F);
  EXPECT_EQ(out[19], 0x10);
  EXPECT_EQ(out[20], 0xFF);
  EXPECT_EQ(out[21], 0x9C);  // -100 = 0xFF9C
  EXPECT_EQ(out[22], 0x12);
  EXPECT_EQ(out[23], 0x34);
  EXPECT_EQ(out[24], 0xFF);
  EXPECT_EQ(out[25], 0xFE);  // -2
  EXPECT_EQ(out[26], 7);
  EXPECT_EQ(out[27], 0);
}

TEST(golden_headers_decode) {
  rapidjson::Document doc;
  const std::string text = ReadText(GoldenPath("audio_header_expected.json"));
  doc.Parse(text.c_str());
  ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
  ASSERT_TRUE(doc.MemberCount() == 4);

  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    const std::string name = it->name.GetString();
    const auto& want = it->value;
    const auto data = ReadBin(GoldenPath(name.c_str()));
    recorder::AudioHeader h;
    const auto err = recorder::DecodeAudioHeader(data.data(), data.size(), 8192, &h);
    EXPECT_TRUE(err == recorder::HeaderError::kOk);
    if (err != recorder::HeaderError::kOk) continue;

    EXPECT_EQ(h.version, static_cast<uint8_t>(want["version"].GetUint()));
    EXPECT_EQ(h.type, static_cast<uint8_t>(want["type"].GetUint()));
    EXPECT_EQ(h.flags, static_cast<uint8_t>(want["flags"].GetUint()));
    EXPECT_EQ(static_cast<uint8_t>(h.format), static_cast<uint8_t>(want["format"].GetUint()));
    EXPECT_EQ(h.segment_id, want["segment_id"].GetUint());
    EXPECT_EQ(h.seq, want["seq"].GetUint());
    EXPECT_EQ(h.timestamp_ms, want["timestamp_ms"].GetUint());
    EXPECT_EQ(h.samples, static_cast<uint16_t>(want["samples"].GetUint()));
    EXPECT_EQ(h.payload_len, static_cast<uint16_t>(want["payload_len"].GetUint()));
    EXPECT_EQ(h.rms_dbfs_x100, static_cast<int16_t>(want["rms_dbfs_x100"].GetInt()));
    EXPECT_EQ(h.peak, static_cast<uint16_t>(want["peak"].GetUint()));
    EXPECT_EQ(h.dc_offset, static_cast<int16_t>(want["dc_offset"].GetInt()));
    EXPECT_EQ(h.vad_score, static_cast<uint8_t>(want["vad_score"].GetUint()));
    EXPECT_EQ(h.reserved, static_cast<uint8_t>(want["reserved"].GetUint()));
    EXPECT_EQ(data.size() - recorder::kAudioHeaderSize,
              static_cast<std::size_t>(want["payload_len_actual"].GetUint()));

    if (h.format == recorder::AudioFormat::kPcmS16le) {
      EXPECT_TRUE(recorder::ValidatePcmFrame(h, 1));
    }
  }
}

TEST(golden_malformed_are_rejected) {
  rapidjson::Document doc;
  const std::string text = ReadText(GoldenPath("malformed_expected.json"));
  doc.Parse(text.c_str());
  ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
  ASSERT_TRUE(doc.MemberCount() == 10);

  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    const std::string name = it->name.GetString();
    const std::string want = it->value.GetString();
    const auto data = ReadBin(GoldenPath(name.c_str()));
    recorder::AudioHeader h;
    const auto err = recorder::DecodeAudioHeader(data.data(), data.size(), 2048, &h);
    EXPECT_TRUE(err != recorder::HeaderError::kOk);
    EXPECT_EQ(std::string(recorder::HeaderErrorCode(err)), want);
  }
}

TEST(header_round_trip) {
  recorder::AudioHeader h;
  h.flags = recorder::AudioFlag(recorder::kFlagPreRoll) |
            recorder::AudioFlag(recorder::kFlagStatsValid);
  h.format = recorder::AudioFormat::kPcmS16le;
  h.segment_id = 11;
  h.seq = 3;
  h.timestamp_ms = 450;
  h.samples = 320;
  h.payload_len = 640;
  h.rms_dbfs_x100 = -1234;
  h.peak = 8000;
  h.dc_offset = -5;
  h.vad_score = 55;

  std::vector<uint8_t> msg(recorder::kAudioHeaderSize + 640, 0);
  recorder::EncodeAudioHeader(h, msg.data());
  recorder::AudioHeader out;
  const auto err = recorder::DecodeAudioHeader(msg.data(), msg.size(), 8192, &out);
  ASSERT_TRUE(err == recorder::HeaderError::kOk);
  EXPECT_EQ(out.segment_id, h.segment_id);
  EXPECT_EQ(out.seq, h.seq);
  EXPECT_EQ(out.timestamp_ms, h.timestamp_ms);
  EXPECT_EQ(out.samples, h.samples);
  EXPECT_EQ(out.payload_len, h.payload_len);
  EXPECT_EQ(out.rms_dbfs_x100, h.rms_dbfs_x100);
  EXPECT_EQ(out.peak, h.peak);
  EXPECT_EQ(out.dc_offset, h.dc_offset);
  EXPECT_EQ(out.vad_score, h.vad_score);
  EXPECT_TRUE(out.HasFlag(recorder::kFlagPreRoll));
  EXPECT_TRUE(out.HasFlag(recorder::kFlagStatsValid));
}

TEST(opus_toc_parses_known_packets) {
  const uint8_t silK20ms[] = {0x48};
  auto info = recorder::ParseOpusToc(silK20ms, 1);
  EXPECT_TRUE(info.ok);
  EXPECT_EQ(info.frame_count, 1);
  EXPECT_EQ(info.total_us, 20000);

  const uint8_t celt10ms[] = {0x90};
  info = recorder::ParseOpusToc(celt10ms, 1);
  EXPECT_TRUE(info.ok);
  EXPECT_EQ(info.total_us, 10000);

  const uint8_t code3_two_frames[] = {0x4B, 0x02};
  info = recorder::ParseOpusToc(code3_two_frames, 2);
  EXPECT_TRUE(info.ok);
  EXPECT_EQ(info.frame_count, 2);
  EXPECT_EQ(info.total_us, 40000);

  const uint8_t code3_zero[] = {0x03, 0x00};
  info = recorder::ParseOpusToc(code3_zero, 2);
  EXPECT_FALSE(info.ok);

  info = recorder::ParseOpusToc(nullptr, 0);
  EXPECT_FALSE(info.ok);
}

TEST(pcm_validation_checks_samples_channels) {
  recorder::AudioHeader h;
  h.format = recorder::AudioFormat::kPcmS16le;
  h.samples = 320;
  h.payload_len = 640;
  EXPECT_TRUE(recorder::ValidatePcmFrame(h, 1));
  EXPECT_FALSE(recorder::ValidatePcmFrame(h, 2));
  h.samples = 160;
  EXPECT_TRUE(recorder::ValidatePcmFrame(h, 2));
  EXPECT_FALSE(recorder::ValidatePcmFrame(h, 0));
}
