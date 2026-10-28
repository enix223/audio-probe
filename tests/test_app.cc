#include "cl_test.h"

#include <memory>
#include <string>
#include <vector>

#include "cl_recorder_app.h"
#include "cl_recorder_protocol.h"
#include "fakes.h"

namespace {

std::string HelloAckJson() {
  return R"({
    "type":"hello_ack","protocol_version":1,
    "session_id":"sess-1","capture_id":"cap-1","server_time_ms":1,
    "control":{"owner":"mqtt","allow_ws_capture_control":false,"ws_mode":"on_demand"},
    "audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"frame_ms":20},
    "capture":{"mode":"continuous","pre_roll_ms":0,"max_segment_ms":30000,"segment_ms":1000,
      "vad":{"engine":"server","onset_ms":120,"silence_ms":700,"min_speech_ms":150,
             "threshold_db":6}},
    "telemetry":{"frame_stats":true,"level_interval_ms":500,"stats_interval_ms":5000},
    "limits":{"max_frame_bytes":8192,"max_queue_ms":3000},
    "start":false
  })";
}

std::string MqttStart(const std::string& id, const std::string& mode, bool expired = false) {
  std::string s = R"({"v":1,"type":"recorder_cmd","id":")" + id +
                  R"(","action":"start")";
  if (expired) s += R"(,"expires_at_ms":1)";
  s += R"(,"params":{"mode":")" + mode + R"(","label":"unit"}})";
  return s;
}

std::string MqttStop(const std::string& id) {
  return R"({"v":1,"type":"recorder_cmd","id":")" + id + R"(","action":"stop"})";
}

recorder::Capabilities Caps() {
  recorder::Capabilities c;
  c.pcm_s16le = true;
  c.pcm.sample_rates = {16000, 48000};
  c.pcm.channels = {1, 2};
  c.pcm.frame_ms = {10, 20, 40, 60};
  c.pcm.payload_max = 4096;
  c.opus = true;
  c.opus_cap.sample_rates = {16000, 48000};
  c.opus_cap.channels = {1, 2};
  c.opus_cap.frame_ms = {10, 20, 40, 60};
  c.opus_cap.payload_max = 255;
  c.opus_cap.encoders = {"host"};
  c.vad = {"server", "energy"};
  c.test_tone = false;
  c.speaker = false;
  c.loopback = false;
  c.device_pre_roll_max_ms = 5000;
  c.server_pre_roll = true;
  c.max_segment_ms = 30000;
  c.tx_queue_ms = 3000;
  c.max_frame_bytes = 8192;
  return c;
}

struct Fixture {
  test::FakeClock clock;
  test::FakeSource source;
  test::FakeMqtt mqtt;
  test::FakeWs* ws = nullptr;
  recorder::RecorderClient client;
  recorder::RecorderApp app;

  Fixture()
      : client([this]() {
          auto p = std::make_unique<test::FakeWs>();
          ws = p.get();
          return p;
        }),
        app(MakeDeps()) {}

  recorder::AppDependencies MakeDeps() {
    recorder::AppDependencies d;
    d.source = &source;
    d.client = &client;
    d.mqtt = &mqtt;
    d.clock = &clock;
    d.device.id = "unit";
    d.device.model = "linux-pcm";
    d.device.firmware = "0.1.0";
    d.device.chip = "linux";
    d.device.audio_frontend = "miniaudio";
    d.capabilities = Caps();
    d.control.owner = "mqtt";
    d.control.allow_ws_capture_control = false;
    d.control.ws_mode = "on_demand";
    d.control.mqtt_lost_stop_timeout_ms = 30000;
    d.control.max_capture_ms = 1800000;
    d.control.ws_idle_timeout_ms = 10000;
    d.audio.format = recorder::AudioFormat::kPcmS16le;
    d.audio.sample_rate = 16000;
    d.audio.channels = 1;
    d.audio.frame_ms = 20;
    d.capture.mode = recorder::CaptureMode::kContinuous;
    d.capture.segment_ms = 1000;
    d.capture.max_segment_ms = 30000;
    d.capture.vad.engine = recorder::VadEngine::kServer;
    d.telemetry.frame_stats = true;
    d.telemetry.level_interval_ms = 500;
    d.telemetry.stats_interval_ms = 5000;
    d.limits.max_frame_bytes = 8192;
    d.limits.max_queue_ms = 3000;
    d.ws_url = "ws://unit";
    d.default_label = "unit";
    return d;
  }

  bool Init() { return app.Init(); }

  void StartViaMqtt(const std::string& id = "c1") {
    mqtt.Push(MqttStart(id, "continuous"));
    app.Poll();  // connect
    app.Poll();  // hello
    if (ws != nullptr) ws->InjectText(HelloAckJson());
    app.Poll();  // handshake + start
  }

  void Feed(int frames) {
    source.available += frames;
    while (source.available > 0) app.Poll();
  }
};

}  // namespace

TEST(app_mqtt_start_streams_and_stop_finalizes) {
  Fixture f;
  ASSERT_TRUE(f.Init());
  f.StartViaMqtt();
  ASSERT_TRUE(f.app.capturing());
  ASSERT_TRUE(f.ws != nullptr);
  EXPECT_TRUE(f.ws->HasText("\"type\":\"capture_state\""));
  EXPECT_TRUE(f.ws->HasText("\"capturing\""));
  ASSERT_TRUE(!f.mqtt.statuses.empty());
  EXPECT_TRUE(f.mqtt.statuses.back().find("\"state\":\"capturing\"") != std::string::npos);
  EXPECT_TRUE(f.mqtt.statuses.back().find("\"capture_id\":\"cap-1\"") != std::string::npos);

  f.Feed(10);
  ASSERT_TRUE(f.ws->binaries.size() == 10);
  recorder::AudioHeader h;
  ASSERT_TRUE(recorder::DecodeAudioHeader(f.ws->binaries.front().data(),
                                          f.ws->binaries.front().size(), 8192, &h) ==
              recorder::HeaderError::kOk);
  EXPECT_EQ(h.segment_id, 1u);
  EXPECT_EQ(h.seq, 0u);
  EXPECT_EQ(h.timestamp_ms, 0u);
  ASSERT_TRUE(recorder::DecodeAudioHeader(f.ws->binaries.back().data(),
                                          f.ws->binaries.back().size(), 8192, &h) ==
              recorder::HeaderError::kOk);
  EXPECT_EQ(h.seq, 9u);
  EXPECT_EQ(h.timestamp_ms, 180u);
  EXPECT_TRUE(f.ws->HasText("\"type\":\"segment_start\""));
  EXPECT_EQ(f.source.starts, 1);

  f.mqtt.Push(MqttStop("c2"));
  f.app.Poll();
  EXPECT_FALSE(f.app.capturing());
  EXPECT_EQ(f.source.stops, 1);
  EXPECT_TRUE(f.ws->HasText("\"type\":\"segment_end\""));
  EXPECT_TRUE(f.ws->HasText("\"reason\":\"stop_capture\""));

  // The drain phase publishes idle after the WS idle timeout.
  f.clock.Advance(10001);
  f.app.Poll();
  ASSERT_TRUE(!f.mqtt.statuses.empty());
  EXPECT_TRUE(f.mqtt.statuses.back().find("\"state\":\"idle\"") != std::string::npos);
}

TEST(app_ws_capture_control_is_nacked_under_mqtt) {
  Fixture f;
  ASSERT_TRUE(f.Init());
  f.StartViaMqtt();
  ASSERT_TRUE(f.app.capturing());

  f.ws->InjectText(R"({"type":"start_capture","id":3})");
  f.app.Poll();
  EXPECT_TRUE(f.ws->HasText("\"code\":\"control_owned_by_mqtt\""));

  f.ws->InjectText(R"({"type":"stop_capture","id":4})");
  f.app.Poll();
  EXPECT_TRUE(f.app.capturing());
}

TEST(app_duplicate_mqtt_start_is_idempotent) {
  Fixture f;
  ASSERT_TRUE(f.Init());
  f.StartViaMqtt();
  ASSERT_TRUE(f.app.capturing());
  EXPECT_EQ(f.source.starts, 1);

  f.mqtt.Push(MqttStart("c1", "continuous"));
  f.app.Poll();
  EXPECT_EQ(f.source.starts, 1);
  EXPECT_TRUE(f.app.capturing());
  ASSERT_TRUE(!f.mqtt.statuses.empty());
  EXPECT_TRUE(f.mqtt.statuses.back().find("\"state\":\"capturing\"") != std::string::npos);
}

TEST(app_expired_mqtt_start_is_rejected) {
  Fixture f;
  ASSERT_TRUE(f.Init());
  f.mqtt.Push(MqttStart("c9", "continuous", true));
  f.app.Poll();
  EXPECT_EQ(f.source.starts, 0);
  bool saw_expired = false;
  for (const auto& s : f.mqtt.statuses) {
    if (s.find("\"code\":\"expired\"") != std::string::npos) saw_expired = true;
  }
  EXPECT_TRUE(saw_expired);
}

TEST(app_mqtt_loss_stops_capture_after_timeout) {
  Fixture f;
  ASSERT_TRUE(f.Init());
  f.StartViaMqtt();
  ASSERT_TRUE(f.app.capturing());

  f.mqtt.connected = false;
  f.app.Poll();  // records the loss time
  EXPECT_TRUE(f.app.capturing());
  f.clock.Advance(30001);
  f.app.Poll();
  EXPECT_FALSE(f.app.capturing());
  ASSERT_TRUE(!f.mqtt.statuses.empty());
  EXPECT_TRUE(f.mqtt.statuses.back().find("mqtt_unavailable") != std::string::npos);
}

TEST(app_max_capture_stops_recording) {
  Fixture f;
  ASSERT_TRUE(f.Init());
  f.StartViaMqtt();
  ASSERT_TRUE(f.app.capturing());
  f.clock.Advance(1800001);
  f.app.Poll();
  EXPECT_FALSE(f.app.capturing());
  ASSERT_TRUE(!f.mqtt.statuses.empty());
  EXPECT_TRUE(f.mqtt.statuses.back().find("max_capture") != std::string::npos);
}

TEST(app_telemetry_is_sent_while_capturing) {
  Fixture f;
  ASSERT_TRUE(f.Init());
  f.StartViaMqtt();
  ASSERT_TRUE(f.app.capturing());
  f.clock.Advance(500);
  f.app.Poll();
  EXPECT_TRUE(f.ws->HasText("\"type\":\"telemetry\""));
}

TEST(app_continuous_segments_reset_seq) {
  Fixture f;
  ASSERT_TRUE(f.Init());
  f.StartViaMqtt();
  ASSERT_TRUE(f.app.capturing());
  f.Feed(60);
  ASSERT_TRUE(f.ws->binaries.size() == 60);

  recorder::AudioHeader a;
  recorder::AudioHeader b;
  ASSERT_TRUE(recorder::DecodeAudioHeader(f.ws->binaries[49].data(),
                                          f.ws->binaries[49].size(), 8192, &a) ==
              recorder::HeaderError::kOk);
  ASSERT_TRUE(recorder::DecodeAudioHeader(f.ws->binaries[50].data(),
                                          f.ws->binaries[50].size(), 8192, &b) ==
              recorder::HeaderError::kOk);
  EXPECT_EQ(a.seq, 49u);
  EXPECT_EQ(a.segment_id, 1u);
  EXPECT_EQ(b.seq, 0u);
  EXPECT_EQ(b.segment_id, 2u);
}
