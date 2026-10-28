#include "cl_linux_audio_source.h"

#include <miniaudio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace recorder {

class LinuxAudioSource::Ring {
 public:
  explicit Ring(std::size_t capacity_samples) {
    std::size_t cap = 1;
    while (cap < capacity_samples) cap <<= 1;
    buf_.assign(cap, 0);
    mask_ = cap - 1;
  }

  void Write(const int16_t* data, std::size_t samples) {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t r = read_.load(std::memory_order_acquire);
    const std::size_t free_samples = buf_.size() - (w - r);
    const std::size_t n = std::min(samples, free_samples);
    if (n > 0) {
      const std::size_t start = w & mask_;
      const std::size_t to_end = buf_.size() - start;
      const std::size_t first = std::min(n, to_end);
      std::memcpy(buf_.data() + start, data, first * sizeof(int16_t));
      if (n > first) {
        std::memcpy(buf_.data(), data + first, (n - first) * sizeof(int16_t));
      }
      write_.store(w + n, std::memory_order_release);
    }
    if (n < samples) dropped_.fetch_add(samples - n, std::memory_order_relaxed);
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

  std::size_t Level() const {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
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

void CaptureCallback(ma_device* device, void* output, const void* input,
                     ma_uint32 frame_count) {
  (void)output;
  auto* self = static_cast<LinuxAudioSource*>(device->pUserData);
  if (self == nullptr || input == nullptr) return;
  self->OnCapture(static_cast<const int16_t*>(input),
                  static_cast<std::size_t>(frame_count) *
                      static_cast<std::size_t>(device->capture.channels));
}

}  // namespace

LinuxAudioSource::~LinuxAudioSource() {
  Stop();
  DestroyDevice();
  delete ring_;
  ring_ = nullptr;
}

void LinuxAudioSource::DestroyDevice() {
  if (device_ != nullptr) {
    auto* dev = static_cast<ma_device*>(device_);
    ma_device_uninit(dev);
    delete dev;
    device_ = nullptr;
  }
  started_ = false;
}

AvpErr LinuxAudioSource::Configure(const AudioConfig& cfg) {
  DestroyDevice();
  delete ring_;
  ring_ = nullptr;

  if (cfg.sample_rate <= 0 || cfg.channels <= 0 || cfg.frame_ms <= 0) {
    return AvpErr::kErrInvalidArg;
  }
  cfg_ = cfg;
  frame_samples_ = static_cast<std::size_t>(cfg.sample_rate) *
                   static_cast<std::size_t>(cfg.frame_ms) / 1000;
  frame_values_ = frame_samples_ * static_cast<std::size_t>(cfg.channels);
  if (frame_values_ == 0) return AvpErr::kErrInvalidArg;

  accum_.assign(frame_values_, 0);
  accum_pos_ = 0;
  frame_buf_.assign(frame_values_, 0);

  // Two seconds of interleaved samples; the callback never blocks.
  ring_ = new Ring(static_cast<std::size_t>(cfg.sample_rate) *
                   static_cast<std::size_t>(cfg.channels) * 2);

  auto* dev = new ma_device;
  ma_device_config config = ma_device_config_init(ma_device_type_capture);
  config.capture.format = ma_format_s16;
  config.capture.channels = static_cast<ma_uint32>(cfg.channels);
  config.sampleRate = static_cast<ma_uint32>(cfg.sample_rate);
  config.dataCallback = CaptureCallback;
  config.pUserData = this;

  const ma_result result = ma_device_init(nullptr, &config, dev);
  if (result != MA_SUCCESS) {
    delete dev;
    return AvpErr::kErrFail;
  }
  if (dev->sampleRate != static_cast<ma_uint32>(cfg.sample_rate) ||
      dev->capture.channels != static_cast<ma_uint32>(cfg.channels)) {
    ma_device_uninit(dev);
    delete dev;
    return AvpErr::kErrNotSupported;
  }

  device_ = dev;
  frame_count_ = 0;
  emitted_samples_ = 0;
  overruns_ = 0;
  errors_ = 0;
  last_ring_dropped_ = 0;
  return AvpErr::kOk;
}

AvpErr LinuxAudioSource::Start() {
  if (device_ == nullptr) return AvpErr::kErrNotInit;
  if (started_) return AvpErr::kOk;
  auto* dev = static_cast<ma_device*>(device_);
  if (ma_device_start(dev) != MA_SUCCESS) return AvpErr::kErrFail;
  accum_pos_ = 0;
  frame_count_ = 0;
  emitted_samples_ = 0;
  last_ring_dropped_ = ring_ != nullptr ? ring_->Dropped() : 0;
  started_ = true;
  return AvpErr::kOk;
}

void LinuxAudioSource::Stop() {
  if (device_ != nullptr && started_) {
    ma_device_stop(static_cast<ma_device*>(device_));
  }
  started_ = false;
}

void LinuxAudioSource::OnCapture(const int16_t* data, std::size_t samples) {
  if (ring_ != nullptr) ring_->Write(data, samples);
}

AvpErr LinuxAudioSource::Read(CaptureFrame& out, uint32_t timeout_ms) {
  out = CaptureFrame{};
  if (!started_ || ring_ == nullptr) return AvpErr::kErrNotInit;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (accum_pos_ < frame_values_) {
    const std::size_t want = frame_values_ - accum_pos_;
    const std::size_t got = ring_->Read(accum_.data() + accum_pos_, want);
    accum_pos_ += got;
    if (accum_pos_ >= frame_values_) break;
    if (timeout_ms == 0) return AvpErr::kErrTimeout;
    if (std::chrono::steady_clock::now() >= deadline) return AvpErr::kErrTimeout;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  std::memcpy(frame_buf_.data(), accum_.data(), frame_values_ * sizeof(int16_t));
  const std::size_t left = accum_pos_ - frame_values_;
  if (left > 0) {
    std::memmove(accum_.data(), accum_.data() + frame_values_, left * sizeof(int16_t));
  }
  accum_pos_ = left;

  out.data = reinterpret_cast<const uint8_t*>(frame_buf_.data());
  out.bytes = frame_values_ * sizeof(int16_t);
  out.samples = frame_samples_;
  out.pcm = frame_buf_.data();
  out.pcm_samples = frame_samples_;
  out.format = AudioFormat::kPcmS16le;
  out.timestamp_ms = static_cast<uint32_t>(emitted_samples_ * 1000 /
                                           static_cast<uint64_t>(cfg_.sample_rate));
  emitted_samples_ += frame_samples_;
  out.stats_valid = false;

  const uint64_t dropped = ring_->Dropped();
  if (dropped != last_ring_dropped_) {
    out.discontinuity = true;
    out.capture_overrun = true;
    ++overruns_;
    last_ring_dropped_ = dropped;
  }
  ++frame_count_;
  return AvpErr::kOk;
}

SourceStats LinuxAudioSource::Stats() const {
  SourceStats s;
  s.frames = frame_count_;
  s.overruns = overruns_;
  s.dropped = ring_ != nullptr ? ring_->Dropped() : 0;
  s.errors = errors_;
  return s;
}

}  // namespace recorder
