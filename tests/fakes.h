#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "cl_audio_source.h"
#include "cl_mqtt_control.h"
#include "cl_recorder_client.h"
#include "cl_recorder_ws.h"

namespace test {

class FakeClock : public recorder::Clock {
 public:
  int64_t now_ms = 1000;
  int64_t MonotonicMs() const override { return now_ms; }
  int64_t MonotonicUs() const override { return now_ms * 1000; }
  int64_t WallMs() const override { return 1760000000000 + now_ms; }
  void Advance(int64_t ms) { now_ms += ms; }
};

class FakeSource : public recorder::AudioSource {
 public:
  recorder::AudioConfig cfg;
  bool started = false;
  int starts = 0;
  int stops = 0;
  uint32_t frame_index = 0;
  int available = 0;
  bool speech = false;
  std::vector<int16_t> pcm;

  recorder::AvpErr Configure(const recorder::AudioConfig& c) override {
    cfg = c;
    pcm.assign(c.FrameSamples() * static_cast<std::size_t>(c.channels), 0);
    return recorder::AvpErr::kOk;
  }
  recorder::AvpErr Start() override {
    started = true;
    ++starts;
    frame_index = 0;
    return recorder::AvpErr::kOk;
  }
  void Stop() override {
    if (started) ++stops;
    started = false;
  }
  recorder::AudioFormat NativeFormat() const override {
    return recorder::AudioFormat::kPcmS16le;
  }
  bool ProvidesPcm() const override { return true; }
  recorder::AvpErr Read(recorder::CaptureFrame& out, uint32_t /*timeout_ms*/) override {
    out = recorder::CaptureFrame{};
    if (!started || available <= 0) return recorder::AvpErr::kErrTimeout;
    --available;
    const std::size_t samples = pcm.size() / static_cast<std::size_t>(cfg.channels);
    if (speech) {
      for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<int16_t>((i % 32 < 16) ? 8000 : -8000);
      }
    }
    out.data = reinterpret_cast<const uint8_t*>(pcm.data());
    out.bytes = pcm.size() * sizeof(int16_t);
    out.samples = samples;
    out.pcm = pcm.data();
    out.pcm_samples = samples;
    out.format = recorder::AudioFormat::kPcmS16le;
    out.timestamp_ms = frame_index * static_cast<uint32_t>(cfg.frame_ms);
    ++frame_index;
    return recorder::AvpErr::kOk;
  }
  recorder::SourceStats Stats() const override {
    recorder::SourceStats s;
    s.frames = frame_index;
    return s;
  }
};

class FakeMqtt : public recorder::MqttControl {
 public:
  bool connected = true;
  bool connect_event = false;
  std::deque<std::string> commands;
  std::vector<std::string> statuses;

  void Push(const std::string& json) { commands.push_back(json); }
  bool Init() override { return true; }
  bool IsCloudConnected() const override { return connected; }
  void PublishStatus(const std::string& json) override { statuses.push_back(json); }
  bool TakeCommand(std::string* json) override {
    if (commands.empty()) return false;
    *json = commands.front();
    commands.pop_front();
    return true;
  }
  bool TakeConnectedEvent() override {
    const bool v = connect_event;
    connect_event = false;
    return v;
  }
};

class FakeWs : public recorder::RecorderWs {
 public:
  recorder::AvpErr Init(const recorder::WsConfig&, const recorder::WsCallbacks& cb) override {
    callbacks = cb;
    return recorder::AvpErr::kOk;
  }
  recorder::AvpErr Connect() override {
    connected = true;
    if (callbacks.on_connected) callbacks.on_connected();
    return recorder::AvpErr::kOk;
  }
  void Disconnect() override {
    if (connected && callbacks.on_disconnected) callbacks.on_disconnected();
    connected = false;
  }
  bool IsConnected() const override { return connected; }
  recorder::AvpErr SendText(const char* data, size_t len) override {
    texts.emplace_back(data, len);
    return recorder::AvpErr::kOk;
  }
  recorder::AvpErr SendBinary(const uint8_t* data, size_t len) override {
    binaries.emplace_back(data, data + len);
    return recorder::AvpErr::kOk;
  }
  std::size_t PendingBytes() const override { return 0; }

  void InjectText(const std::string& text) {
    if (callbacks.on_text) callbacks.on_text(text.data(), text.size());
  }
  bool HasText(const std::string& needle) const {
    for (const auto& t : texts) {
      if (t.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  void Clear() {
    texts.clear();
    binaries.clear();
  }

  recorder::WsCallbacks callbacks;
  bool connected = false;
  std::vector<std::string> texts;
  std::vector<std::vector<uint8_t>> binaries;
};

}  // namespace test
