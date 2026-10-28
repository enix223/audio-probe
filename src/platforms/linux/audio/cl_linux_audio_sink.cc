#include "cl_linux_audio_sink.h"

#include <miniaudio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace recorder {

class LinuxAudioSink::Ring {
 public:
  explicit Ring(std::size_t capacity_samples) {
    std::size_t cap = 1;
    while (cap < capacity_samples) cap <<= 1;
    buf_.assign(cap, 0);
    mask_ = cap - 1;
  }

  std::size_t Write(const int16_t* data, std::size_t samples) {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t r = read_.load(std::memory_order_acquire);
    const std::size_t free_samples = buf_.size() - (w - r);
    const std::size_t n = std::min(samples, free_samples);
    if (n > 0) {
      const std::size_t start = w & mask_;
      const std::size_t to_end = buf_.size() - start;
      const std::size_t first = std::min(n, to_end);
      std::memcpy(buf_.data() + start, data, first * sizeof(int16_t));
      if (n > first) std::memcpy(buf_.data(), data + first, (n - first) * sizeof(int16_t));
      write_.store(w + n, std::memory_order_release);
    }
    if (n < samples) dropped_.fetch_add(samples - n, std::memory_order_relaxed);
    return n;
  }

  std::size_t Read(int16_t* data, std::size_t samples) {
    const std::size_t r = read_.load(std::memory_order_relaxed);
    const std::size_t w = write_.load(std::memory_order_acquire);
    const std::size_t avail = w - r;
    const std::size_t n = std::min(samples, avail);
    if (n > 0) {
      const std::size_t start = r & mask_;
      const std::size_t to_end = buf_.size() - start;
      const std::size_t first = std::min(n, to_end);
      std::memcpy(data, buf_.data() + start, first * sizeof(int16_t));
      if (n > first) std::memcpy(data + first, buf_.data(), (n - first) * sizeof(int16_t));
      read_.store(r + n, std::memory_order_release);
    }
    return n;
  }

  uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  std::vector<int16_t> buf_;
  std::size_t mask_ = 0;
  std::atomic<std::size_t> write_{0};
  std::atomic<std::size_t> read_{0};
  std::atomic<uint64_t> dropped_{0};
};

namespace {

void PlaybackCallback(ma_device* device, void* output, const void* input,
                      ma_uint32 frame_count) {
  (void)input;
  auto* self = static_cast<LinuxAudioSink*>(device->pUserData);
  if (self == nullptr || output == nullptr) return;
  self->OnPlayback(static_cast<int16_t*>(output),
                   static_cast<std::size_t>(frame_count) *
                       static_cast<std::size_t>(device->playback.channels));
}

}  // namespace

LinuxAudioSink::~LinuxAudioSink() { Deinit(); }

AvpErr LinuxAudioSink::Init(int sample_rate, int channels) {
  Deinit();
  if (sample_rate <= 0 || channels <= 0) return AvpErr::kErrInvalidArg;
  sample_rate_ = sample_rate;
  channels_ = channels;

  ring_ = new Ring(static_cast<std::size_t>(sample_rate) *
                   static_cast<std::size_t>(channels) * 2);  // 2 s

  auto* dev = new ma_device;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_s16;
  config.playback.channels = static_cast<ma_uint32>(channels);
  config.sampleRate = static_cast<ma_uint32>(sample_rate);
  config.dataCallback = PlaybackCallback;
  config.pUserData = this;

  if (ma_device_init(nullptr, &config, dev) != MA_SUCCESS) {
    delete dev;
    delete ring_;
    ring_ = nullptr;
    return AvpErr::kErrFail;
  }
  device_ = dev;
  if (ma_device_start(dev) != MA_SUCCESS) {
    ma_device_uninit(dev);
    delete dev;
    device_ = nullptr;
    delete ring_;
    ring_ = nullptr;
    return AvpErr::kErrFail;
  }
  return AvpErr::kOk;
}

void LinuxAudioSink::Deinit() {
  if (device_ != nullptr) {
    auto* dev = static_cast<ma_device*>(device_);
    ma_device_stop(dev);
    ma_device_uninit(dev);
    delete dev;
    device_ = nullptr;
  }
  delete ring_;
  ring_ = nullptr;
}

AvpErr LinuxAudioSink::Write(const int16_t* pcm, std::size_t frames, int timeout_ms) {
  if (device_ == nullptr || ring_ == nullptr) return AvpErr::kErrNotInit;
  const std::size_t samples = frames * static_cast<std::size_t>(channels_);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  std::size_t written = 0;
  while (written < samples) {
    const std::size_t n = ring_->Write(pcm + written, samples - written);
    written += n;
    if (written >= samples) break;
    if (timeout_ms == 0) return AvpErr::kErrBusy;
    if (std::chrono::steady_clock::now() >= deadline) return AvpErr::kErrTimeout;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return AvpErr::kOk;
}

void LinuxAudioSink::SetVolume(int percent) {
  if (device_ == nullptr) return;
  const float v = static_cast<float>(std::max(0, std::min(100, percent))) / 100.0f;
  ma_device_set_master_volume(static_cast<ma_device*>(device_), v);
}

uint64_t LinuxAudioSink::dropped_samples() const {
  return ring_ != nullptr ? ring_->Dropped() : 0;
}

void LinuxAudioSink::OnPlayback(int16_t* output, std::size_t samples) {
  const std::size_t got = ring_ != nullptr ? ring_->Read(output, samples) : 0;
  if (got < samples) {
    std::memset(output + got, 0, (samples - got) * sizeof(int16_t));
  }
}

}  // namespace recorder
