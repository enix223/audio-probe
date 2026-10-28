#include "cl_test.h"

#include <string>

#include "cl_recorder_messages.h"
#include "rapidjson/document.h"

namespace {

recorder::Capabilities LinuxCaps() {
  recorder::Capabilities c;
  c.pcm_s16le = true;
  c.pcm.sample_rates = {16000, 48000};
  c.pcm.channels = {1, 2};
  c.pcm.frame_ms = {10, 20, 40, 60};
  c.pcm.payload_max = 4096;
  c.pcm.channel_map = {"mic0"};
  c.opus = true;
  c.opus_cap.sample_rates = {16000, 48000};
  c.opus_cap.channels = {1, 2};
  c.opus_cap.frame_ms = {10, 20, 40, 60};
  c.opus_cap.bitrate = {6000, 64000};
  c.opus_cap.payload_max = 255;
  c.opus_cap.encoders = {"host"};
  c.vad = {"server", "energy"};
  c.test_tone = true;
  c.tone_waveforms = {"sine", "square", "sweep"};
  c.tone_min_hz = 20;
  c.tone_max_hz = 20000;
  c.speaker = true;
  c.device_pre_roll_max_ms = 5000;
  c.server_pre_roll = true;
  c.max_segment_ms = 30000;
  c.tx_queue_ms = 3000;
  c.max_frame_bytes = 8192;
  return c;
}

recorder::AudioConfig PcmAudio() {
  recorder::AudioConfig a;
  a.format = recorder::AudioFormat::kPcmS16le;
  a.sample_rate = 16000;
  a.channels = 1;
  a.frame_ms = 20;
  return a;
}

void Parse(const std::string& text, rapidjson::Document* doc) {
  doc->Parse(text.c_str());
}

}  // namespace

TEST(build_hello_contains_capabilities_and_control) {
  recorder::HelloContext ctx;
  ctx.device.id = "recorder-linux";
  ctx.device.model = "linux-pcm";
  ctx.device.firmware = "0.1.0";
  ctx.device.chip = "linux";
  ctx.device.audio_frontend = "miniaudio";
  ctx.device.boot_id = "abcd1234";
  ctx.capabilities = LinuxCaps();
  ctx.control.owner = "mqtt";
  ctx.control.ws_mode = "on_demand";
  ctx.control.mqtt_lost_stop_timeout_ms = 30000;
  ctx.control.max_capture_ms = 1800000;
  ctx.clock.monotonic_ms = 1234;
  ctx.clock.wall_ms = 1760000000000;
  ctx.clock.synced = true;

  const std::string json = recorder::BuildHello(ctx);
  rapidjson::Document doc;
  Parse(json, &doc);
  ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("hello"));
  EXPECT_EQ(doc["protocol_version"].GetInt(), 1);
  EXPECT_EQ(std::string(doc["device"]["id"].GetString()), std::string("recorder-linux"));
  EXPECT_EQ(std::string(doc["control"]["owner"].GetString()), std::string("mqtt"));
  EXPECT_FALSE(doc["control"]["allow_ws_capture_control"].GetBool());
  const auto& formats = doc["capabilities"]["formats"];
  EXPECT_TRUE(formats.HasMember("pcm_s16le"));
  EXPECT_TRUE(formats.HasMember("opus"));
  EXPECT_TRUE(formats["opus"]["encoders"].IsArray());
  EXPECT_EQ(std::string(formats["opus"]["encoders"][0].GetString()), std::string("host"));
  EXPECT_TRUE(doc["capabilities"]["vad"].IsArray());
  EXPECT_EQ(doc["capabilities"]["limits"]["max_frame_bytes"].GetInt(), 8192);
  EXPECT_TRUE(doc["capabilities"]["test_tone"].IsObject());
}

TEST(build_capture_state_and_segment_messages) {
  const auto audio = PcmAudio();
  recorder::CaptureConfig capture;
  capture.mode = recorder::CaptureMode::kVad;
  capture.pre_roll_ms = 300;
  capture.max_segment_ms = 15000;
  capture.vad.engine = recorder::VadEngine::kEnergy;

  const std::string state = recorder::BuildCaptureState(
      "capturing", "cap-1", "vad", &audio, &capture, false, 0, 40, 3, 1, 2);
  rapidjson::Document doc;
  Parse(state, &doc);
  ASSERT_TRUE(!doc.HasParseError());
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("capture_state"));
  EXPECT_EQ(std::string(doc["state"].GetString()), std::string("capturing"));
  EXPECT_EQ(std::string(doc["capture_id"].GetString()), std::string("cap-1"));
  EXPECT_TRUE(doc["gain_db"].IsNull());
  EXPECT_EQ(doc["tx"]["queue_ms"].GetInt(), 40);
  EXPECT_EQ(doc["tx"]["reconnects"].GetInt(), 2);

  recorder::SegmentStartEvent start;
  start.segment_id = 7;
  start.reason = "vad";
  start.t_ms = 1000;
  start.onset_t_ms = 1300;
  start.pre_roll_ms = 300;
  start.label = "run-1";
  const std::string start_json = recorder::BuildSegmentStart(start, "cap-1", audio, false, 0);
  Parse(start_json, &doc);
  ASSERT_TRUE(!doc.HasParseError());
  EXPECT_EQ(doc["segment_id"].GetUint(), 7u);
  EXPECT_EQ(doc["onset_t_ms"].GetUint(), 1300u);
  EXPECT_TRUE(doc["tone"].IsNull());
  EXPECT_EQ(std::string(doc["audio"]["format"].GetString()), std::string("pcm_s16le"));

  recorder::SegmentEndEvent end;
  end.segment_id = 7;
  end.reason = "silence";
  end.t_ms = 2500;
  end.frames = 75;
  end.pre_roll_frames = 15;
  end.duration_ms = 1500;
  end.speech_ms = 1100;
  end.rms_dbfs = -21.5;
  end.peak_dbfs = -3.0;
  const std::string end_json = recorder::BuildSegmentEnd(end, "cap-1");
  Parse(end_json, &doc);
  ASSERT_TRUE(!doc.HasParseError());
  EXPECT_EQ(std::string(doc["reason"].GetString()), std::string("silence"));
  EXPECT_EQ(doc["frames"].GetInt(), 75);
  EXPECT_EQ(doc["pre_roll_frames"].GetInt(), 15);
}

TEST(build_segment_start_with_tone_metadata) {
  recorder::SegmentStartEvent start;
  start.segment_id = 9;
  start.reason = "test_tone";
  start.t_ms = 100;
  start.onset_t_ms = 100;
  start.has_tone = true;
  start.tone.tone_id = "thd-1k";
  start.tone.waveform = "sine";
  start.tone.frequency_hz = 1000;
  start.tone.amplitude_dbfs = -6;
  start.tone.speaker_volume = 60;
  start.tone.fade_ms = 20;
  const std::string json =
      recorder::BuildSegmentStart(start, "cap-1", PcmAudio(), false, 0);
  rapidjson::Document doc;
  Parse(json, &doc);
  ASSERT_TRUE(!doc.HasParseError());
  ASSERT_TRUE(doc["tone"].IsObject());
  EXPECT_EQ(std::string(doc["tone"]["tone_id"].GetString()), std::string("thd-1k"));
  EXPECT_NEAR(doc["tone"]["frequency_hz"].GetDouble(), 1000.0, 1e-9);
}

TEST(build_telemetry_null_levels) {
  recorder::TelemetrySnapshot s;
  s.t_ms = 1234;
  s.capture_id = "cap-1";
  s.vad_state = "silence";
  s.vad_score = 5;
  s.has_levels = false;
  s.tx_queue_ms = 0;
  s.uptime_ms = 1234;
  const std::string json = recorder::BuildTelemetry(s);
  rapidjson::Document doc;
  Parse(json, &doc);
  ASSERT_TRUE(!doc.HasParseError());
  EXPECT_TRUE(doc["rms_dbfs"].IsNull());
  EXPECT_TRUE(doc["peak_dbfs"].IsNull());
  EXPECT_TRUE(doc["noise_dbfs"].IsNull());
  EXPECT_TRUE(doc["gain_db"].IsNull());
  EXPECT_EQ(std::string(doc["vad_state"].GetString()), std::string("silence"));
}

TEST(build_responses_and_mqtt_status) {
  rapidjson::Document doc;

  recorder::MqttStatusData d;
  d.state = recorder::RecorderState::kCapturing;
  d.capture_id = "cap-1";
  d.segment_id = 3;
  d.has_audio = true;
  d.audio = PcmAudio();
  d.gain_supported = false;
  d.tx_ms = 20;
  d.drops = 1;
  d.reconnects = 2;
  d.ts_ms = 1760000000000;
  const std::string status = recorder::BuildMqttStatus("c1", d);
  EXPECT_TRUE(status.size() < 1024);
  Parse(status, &doc);
  ASSERT_TRUE(!doc.HasParseError());
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("recorder_status"));
  EXPECT_EQ(std::string(doc["id"].GetString()), std::string("c1"));
  EXPECT_TRUE(doc["gain_db"].IsNull());
  EXPECT_TRUE(doc["error"].IsNull());
  EXPECT_EQ(doc["queue"]["drops"].GetInt(), 1);

  Parse(recorder::BuildAck(true, 7), &doc);
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("ack"));
  EXPECT_EQ(doc["id"].GetInt64(), 7);

  Parse(recorder::BuildNack(true, 7, "unsupported_feature", "nope"), &doc);
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("nack"));
  EXPECT_EQ(std::string(doc["code"].GetString()), std::string("unsupported_feature"));
  EXPECT_FALSE(doc["retryable"].GetBool());

  Parse(recorder::BuildPong(true, 3, 100, 105, true, 1760000000000), &doc);
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("pong"));
  EXPECT_EQ(doc["t0_ms"].GetInt64(), 100);
  EXPECT_EQ(doc["t1_ms"].GetInt64(), 105);

  Parse(recorder::BuildErrorJson("audio_hw_error", "mic failed", true), &doc);
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("error"));
  EXPECT_TRUE(doc["fatal"].GetBool());

  Parse(recorder::BuildToneStart("thd-1k", 100, 9), &doc);
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("tone_start"));
  EXPECT_EQ(doc["segment_id"].GetUint(), 9u);

  recorder::StatsSnapshot stats;
  stats.state = "capturing";
  stats.has_audio = true;
  stats.audio = PcmAudio();
  Parse(recorder::BuildStatsJson(stats), &doc);
  EXPECT_EQ(std::string(doc["type"].GetString()), std::string("stats"));
  EXPECT_EQ(std::string(doc["state"].GetString()), std::string("capturing"));
}
