#include "cl_test.h"

#include <fstream>
#include <string>

#include "cl_recorder_commands.h"
#include "rapidjson/document.h"

namespace {

std::string Golden(const char* name) {
  std::ifstream f(std::string(RECORDER_ROOT) + "/protocol/golden/" + name);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(mqtt_start_parses) {
  const std::string text = Golden("mqtt_start.json");
  recorder::MqttCommand cmd;
  std::string err;
  ASSERT_TRUE(recorder::ParseMqttCommand(text.data(), text.size(), &cmd, &err));
  EXPECT_EQ(cmd.v, 1);
  EXPECT_EQ(cmd.type, std::string("recorder_cmd"));
  EXPECT_EQ(cmd.id, std::string("c1"));
  EXPECT_EQ(cmd.action, std::string("start"));
  EXPECT_TRUE(cmd.has_expires_at);
  EXPECT_TRUE(cmd.expires_at_ms > cmd.issued_at_ms);
  ASSERT_TRUE(cmd.Params() != nullptr);
  EXPECT_TRUE(cmd.HasParam("mode"));

  std::string mode;
  EXPECT_TRUE(recorder::JsonGetString(*cmd.Params(), "mode", &mode));
  EXPECT_EQ(mode, std::string("vad"));

  recorder::AudioConfig audio;
  ASSERT_TRUE(recorder::ParseAudioConfigJson((*cmd.Params())["audio"], &audio, &err));
  EXPECT_TRUE(audio.format == recorder::AudioFormat::kPcmS16le);
  EXPECT_EQ(audio.sample_rate, 16000);
  EXPECT_EQ(audio.channels, 1);
  EXPECT_EQ(audio.frame_ms, 20);
}

TEST(mqtt_stop_and_status_parse) {
  const std::string stop = Golden("mqtt_stop.json");
  recorder::MqttCommand cmd;
  std::string err;
  ASSERT_TRUE(recorder::ParseMqttCommand(stop.data(), stop.size(), &cmd, &err));
  EXPECT_EQ(cmd.action, std::string("stop"));
  EXPECT_EQ(cmd.id, std::string("c2"));
  EXPECT_FALSE(cmd.has_expires_at);

  const std::string status = Golden("mqtt_status.json");
  rapidjson::Document doc;
  doc.Parse(status.c_str());
  ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("recorder_status"));
  EXPECT_EQ(std::string(doc["state"].GetString()), std::string("capturing"));
  EXPECT_TRUE(doc["queue"].IsObject());
  EXPECT_TRUE(doc["error"].IsNull());
}

TEST(mqtt_rejects_malformed_envelopes) {
  recorder::MqttCommand cmd;
  std::string err;
  auto reject = [&](const std::string& text) {
    recorder::MqttCommand local;
    std::string local_err;
    EXPECT_FALSE(recorder::ParseMqttCommand(text.data(), text.size(), &local, &local_err));
  };

  reject("{}");
  reject(R"({"v":2,"type":"recorder_cmd","id":"a","action":"start"})");
  reject(R"({"v":1,"type":"recorder_cmd","action":"start"})");
  reject(R"({"v":1,"type":"recorder_cmd","id":"a","action":"dance"})");
  reject(R"({"v":1,"type":"recorder_cmd","id":"a","action":"start","params":5})");
  reject("not json");
  (void)cmd;
  (void)err;
}

TEST(mqtt_accepts_numeric_and_string_ids) {
  recorder::MqttCommand cmd;
  std::string err;
  const std::string numeric = R"({"v":1,"type":"recorder_cmd","id":42,"action":"status"})";
  ASSERT_TRUE(recorder::ParseMqttCommand(numeric.data(), numeric.size(), &cmd, &err));
  EXPECT_EQ(cmd.id, std::string("42"));
  const std::string textual = R"({"v":1,"type":"recorder_cmd","id":"abc","action":"status"})";
  ASSERT_TRUE(recorder::ParseMqttCommand(textual.data(), textual.size(), &cmd, &err));
  EXPECT_EQ(cmd.id, std::string("abc"));
}

TEST(ws_command_parses_hello_ack_and_id) {
  const std::string ack = Golden("hello_ack_linux_pcm.json");
  recorder::WsCommand cmd;
  std::string err;
  ASSERT_TRUE(recorder::ParseWsCommand(ack.data(), ack.size(), &cmd, &err));
  EXPECT_EQ(cmd.type, std::string("hello_ack"));
  EXPECT_FALSE(cmd.has_id);

  const std::string tone = Golden("test_tone_command.json");
  ASSERT_TRUE(recorder::ParseWsCommand(tone.data(), tone.size(), &cmd, &err));
  EXPECT_EQ(cmd.type, std::string("test_tone"));
  EXPECT_TRUE(cmd.has_id);
  EXPECT_EQ(cmd.id, 16);
}

TEST(capture_and_tone_config_parse) {
  const std::string ack = Golden("hello_ack_linux_pcm.json");
  rapidjson::Document doc;
  doc.Parse(ack.c_str());
  ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());

  recorder::AudioConfig audio;
  std::string err;
  ASSERT_TRUE(recorder::ParseAudioConfigJson(doc["audio"], &audio, &err));
  EXPECT_TRUE(audio.format == recorder::AudioFormat::kPcmS16le);
  EXPECT_EQ(audio.sample_rate, 16000);
  EXPECT_EQ(audio.frame_ms, 20);

  recorder::CaptureConfig capture;
  ASSERT_TRUE(recorder::ParseCaptureConfigJson(doc["capture"], &capture, &err));
  EXPECT_TRUE(capture.mode == recorder::CaptureMode::kVad);
  EXPECT_EQ(capture.pre_roll_ms, 300);
  EXPECT_EQ(capture.max_segment_ms, 15000);
  EXPECT_TRUE(capture.vad.engine == recorder::VadEngine::kEnergy);
  EXPECT_EQ(capture.vad.silence_ms, 700);
  EXPECT_NEAR(capture.vad.threshold_db, 6.0, 1e-9);

  recorder::TelemetryConfig telemetry;
  ASSERT_TRUE(recorder::ParseTelemetryConfigJson(doc["telemetry"], &telemetry, &err));
  EXPECT_TRUE(telemetry.frame_stats);
  EXPECT_EQ(telemetry.level_interval_ms, 500);

  recorder::ToneMetadata tone;
  const std::string tone_text = Golden("test_tone_command.json");
  rapidjson::Document tone_doc;
  tone_doc.Parse(tone_text.c_str());
  ASSERT_TRUE(recorder::ParseToneJson(tone_doc, &tone, &err));
  EXPECT_EQ(tone.tone_id, std::string("thd-1k"));
  EXPECT_EQ(tone.waveform, std::string("sine"));
  EXPECT_NEAR(tone.frequency_hz, 1000.0, 1e-9);
  EXPECT_NEAR(tone.amplitude_dbfs, -6.0, 1e-9);
  EXPECT_EQ(tone.duration_ms, 5000);
  EXPECT_EQ(tone.speaker_volume, 60);
}

TEST(opus_ack_selects_server_vad_continuous) {
  const std::string ack = Golden("hello_ack_ln882h_opus.json");
  rapidjson::Document doc;
  doc.Parse(ack.c_str());
  ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
  recorder::AudioConfig audio;
  recorder::CaptureConfig capture;
  std::string err;
  ASSERT_TRUE(recorder::ParseAudioConfigJson(doc["audio"], &audio, &err));
  ASSERT_TRUE(recorder::ParseCaptureConfigJson(doc["capture"], &capture, &err));
  EXPECT_TRUE(audio.format == recorder::AudioFormat::kOpus);
  EXPECT_EQ(audio.opus_encoder, std::string("gx8006"));
  EXPECT_TRUE(capture.mode == recorder::CaptureMode::kContinuous);
  EXPECT_TRUE(capture.vad.engine == recorder::VadEngine::kServer);
}
