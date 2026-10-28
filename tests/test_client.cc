#include "cl_test.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cl_recorder_client.h"
#include "cl_recorder_protocol.h"

namespace {

class FakeWs : public recorder::RecorderWs {
 public:
  recorder::AvpErr Init(const recorder::WsConfig&, const recorder::WsCallbacks& cb) override {
    callbacks_ = cb;
    return recorder::AvpErr::kOk;
  }
  recorder::AvpErr Connect() override {
    connected_ = true;
    if (callbacks_.on_connected) callbacks_.on_connected();
    return recorder::AvpErr::kOk;
  }
  void Disconnect() override {
    if (connected_ && callbacks_.on_disconnected) callbacks_.on_disconnected();
    connected_ = false;
  }
  bool IsConnected() const override { return connected_; }

  recorder::AvpErr SendText(const char* data, size_t len) override {
    if (fail_text) {
      fail_text = false;
      return recorder::AvpErr::kErrFail;
    }
    texts.emplace_back(data, len);
    return recorder::AvpErr::kOk;
  }
  recorder::AvpErr SendBinary(const uint8_t* data, size_t len) override {
    if (fail_binary) {
      fail_binary = false;
      return recorder::AvpErr::kErrFail;
    }
    binaries.emplace_back(data, data + len);
    return recorder::AvpErr::kOk;
  }
  std::size_t PendingBytes() const override { return 0; }

  void InjectText(const std::string& text) {
    if (callbacks_.on_text) callbacks_.on_text(text.data(), text.size());
  }

  recorder::WsCallbacks callbacks_;
  bool connected_ = false;
  bool fail_text = false;
  bool fail_binary = false;
  std::vector<std::string> texts;
  std::vector<std::vector<uint8_t>> binaries;
};

class FakeClock : public recorder::Clock {
 public:
  int64_t MonotonicMs() const override { return 456; }
  int64_t MonotonicUs() const override { return 456000; }
  int64_t WallMs() const override { return 1760000000000; }
};

recorder::TxRecord AudioRecord(uint32_t seq, std::size_t bytes) {
  recorder::TxRecord r;
  r.kind = recorder::TxRecord::Kind::kAudio;
  r.format = recorder::AudioFormat::kPcmS16le;
  r.segment_id = 1;
  r.seq = seq;
  r.timestamp_ms = seq * 20;
  r.samples = static_cast<uint16_t>(bytes / 2);
  r.frame_ms = 20;
  r.flags = recorder::AudioFlag(recorder::kFlagStatsValid);
  r.payload.assign(bytes, static_cast<uint8_t>(seq));
  return r;
}

}  // namespace

TEST(client_sends_complete_messages) {
  FakeWs* fake = nullptr;
  recorder::RecorderClient client([&]() {
    auto p = std::make_unique<FakeWs>();
    fake = p.get();
    return p;
  });
  ASSERT_TRUE(client.Connect(recorder::WsConfig{}));
  recorder::ClientEvent ev;
  ASSERT_TRUE(client.TakeEvent(&ev));
  EXPECT_TRUE(ev.type == recorder::ClientEventType::kConnected);
  EXPECT_TRUE(client.IsConnected());

  EXPECT_TRUE(client.EnqueueControl(R"({"type":"hello"})"));
  EXPECT_TRUE(client.EnqueueAudio(AudioRecord(0, 640)));
  client.Flush();

  ASSERT_TRUE(fake->texts.size() == 1);
  EXPECT_EQ(fake->texts[0], std::string(R"({"type":"hello"})"));
  ASSERT_TRUE(fake->binaries.size() == 1);
  ASSERT_TRUE(fake->binaries[0].size() == 28 + 640);

  recorder::AudioHeader h;
  const auto err = recorder::DecodeAudioHeader(fake->binaries[0].data(), fake->binaries[0].size(),
                                               8192, &h);
  ASSERT_TRUE(err == recorder::HeaderError::kOk);
  EXPECT_EQ(h.segment_id, 1u);
  EXPECT_EQ(h.seq, 0u);
  EXPECT_EQ(h.payload_len, 640);
  EXPECT_EQ(fake->binaries[0][28], 0);
  EXPECT_EQ(fake->binaries[0][28 + 639], 0);
  client.Disconnect();
}

TEST(client_never_truncates_large_payloads) {
  FakeWs* fake = nullptr;
  recorder::RecorderClient client([&]() {
    auto p = std::make_unique<FakeWs>();
    fake = p.get();
    return p;
  });
  ASSERT_TRUE(client.Connect(recorder::WsConfig{}));
  recorder::ClientEvent ev;
  ASSERT_TRUE(client.TakeEvent(&ev));

  EXPECT_TRUE(client.EnqueueAudio(AudioRecord(0, 4096)));
  client.Flush();
  ASSERT_TRUE(fake->binaries.size() == 1);
  EXPECT_EQ(fake->binaries[0].size(), 28u + 4096u);
  recorder::AudioHeader h;
  ASSERT_TRUE(recorder::DecodeAudioHeader(fake->binaries[0].data(),
                                          fake->binaries[0].size(), 8192, &h) ==
              recorder::HeaderError::kOk);
  EXPECT_EQ(h.payload_len, 4096);
  client.Disconnect();
}

TEST(client_ping_bypasses_the_queue) {
  FakeWs* fake = nullptr;
  FakeClock clock;
  recorder::RecorderClient client([&]() {
    auto p = std::make_unique<FakeWs>();
    fake = p.get();
    return p;
  });
  client.SetClock(&clock);
  ASSERT_TRUE(client.Connect(recorder::WsConfig{}));
  recorder::ClientEvent ev;
  ASSERT_TRUE(client.TakeEvent(&ev));

  fake->InjectText(R"({"type":"ping","id":7,"t0_ms":123})");
  ASSERT_TRUE(fake->texts.size() == 1);
  EXPECT_TRUE(fake->texts[0].find("\"type\":\"pong\"") != std::string::npos);
  EXPECT_TRUE(fake->texts[0].find("\"id\":7") != std::string::npos);
  EXPECT_TRUE(fake->texts[0].find("\"t0_ms\":123") != std::string::npos);
  EXPECT_TRUE(fake->texts[0].find("\"t1_ms\":456") != std::string::npos);
  client.Disconnect();
}

TEST(client_control_send_failure_requeues_the_control_record) {
  FakeWs* fake = nullptr;
  recorder::RecorderClient client([&]() {
    auto p = std::make_unique<FakeWs>();
    fake = p.get();
    return p;
  });
  ASSERT_TRUE(client.Connect(recorder::WsConfig{}));
  recorder::ClientEvent ev;
  ASSERT_TRUE(client.TakeEvent(&ev));

  EXPECT_TRUE(client.EnqueueControl("hello"));
  fake->fail_text = true;
  client.Flush();
  EXPECT_TRUE(fake->texts.empty());
  EXPECT_EQ(client.send_errors(), 1u);

  client.Flush();
  ASSERT_TRUE(fake->texts.size() == 1);
  EXPECT_EQ(fake->texts[0], std::string("hello"));
  client.Disconnect();
}

TEST(client_reports_control_queue_full) {
  FakeWs* fake = nullptr;
  recorder::RecorderClient client([&]() {
    auto p = std::make_unique<FakeWs>();
    fake = p.get();
    return p;
  });
  ASSERT_TRUE(client.Connect(recorder::WsConfig{}));
  recorder::ClientEvent ev;
  ASSERT_TRUE(client.TakeEvent(&ev));

  // The default queue is large; fill it with control records, then one more.
  bool full = false;
  for (int i = 0; i < 1000; ++i) {
    if (!client.EnqueueControl(std::string(2048, 'x'))) {
      full = true;
      break;
    }
  }
  EXPECT_TRUE(full);
  recorder::ClientEvent full_event;
  bool saw = false;
  while (client.TakeEvent(&full_event)) {
    if (full_event.type == recorder::ClientEventType::kControlQueueFull) saw = true;
  }
  EXPECT_TRUE(saw);
  client.Disconnect();
}
