#include "cl_file_audio_source.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace recorder {

namespace {
uint32_t ReadU32Le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t ReadU16Le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
}  // namespace

FileAudioSource::FileAudioSource(std::string path, bool realtime, bool loop)
    : path_(std::move(path)), realtime_(realtime), loop_(loop) {}

AvpErr FileAudioSource::Configure(const AudioConfig& cfg) {
  Stop();
  file_.close();
  cfg_ = cfg;
  if (cfg.sample_rate <= 0 || cfg.channels <= 0 || cfg.frame_ms <= 0) {
    return AvpErr::kErrInvalidArg;
  }

  file_.open(path_, std::ios::binary);
  if (!file_) return AvpErr::kErrFail;

  frame_samples_ = static_cast<std::size_t>(cfg.sample_rate) *
                   static_cast<std::size_t>(cfg.frame_ms) / 1000;
  frame_values_ = frame_samples_ * static_cast<std::size_t>(cfg.channels);
  if (frame_values_ == 0) return AvpErr::kErrInvalidArg;
  frame_buf_.assign(frame_values_, 0);

  wav_ = false;
  data_offset_ = 0;
  data_bytes_ = 0;
  file_rate_ = cfg.sample_rate;
  file_channels_ = cfg.channels;
  file_bits_ = 16;

  uint8_t riff[12] = {};
  file_.read(reinterpret_cast<char*>(riff), sizeof(riff));
  if (file_.gcount() == 12 && std::memcmp(riff, "RIFF", 4) == 0 &&
      std::memcmp(riff + 8, "WAVE", 4) == 0) {
    wav_ = true;
    if (ParseWav() != AvpErr::kOk) return AvpErr::kErrNotSupported;
    if (file_rate_ != cfg.sample_rate || file_channels_ != cfg.channels ||
        file_bits_ != 16) {
      return AvpErr::kErrNotSupported;
    }
  } else {
    file_.clear();
    file_.seekg(0, std::ios::end);
    data_bytes_ = static_cast<std::size_t>(file_.tellg());
    file_.clear();
  }

  samples_emitted_ = 0;
  frames_emitted_ = 0;
  eof_ = false;
  paced_ = false;
  return AvpErr::kOk;
}

AvpErr FileAudioSource::ParseWav() {
  int audio_format = 0;
  while (file_) {
    uint8_t header[8] = {};
    file_.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file_.gcount() != 8) break;
    const uint32_t size = ReadU32Le(header + 4);
    if (std::memcmp(header, "fmt ", 4) == 0) {
      std::vector<uint8_t> fmt(size);
      file_.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(size));
      if (file_.gcount() != static_cast<std::streamsize>(size) || size < 16) return AvpErr::kErrFail;
      audio_format = ReadU16Le(fmt.data());
      file_channels_ = ReadU16Le(fmt.data() + 2);
      file_rate_ = static_cast<int>(ReadU32Le(fmt.data() + 4));
      file_bits_ = ReadU16Le(fmt.data() + 14);
    } else if (std::memcmp(header, "data", 4) == 0) {
      data_offset_ = static_cast<std::size_t>(file_.tellg());
      file_.seekg(0, std::ios::end);
      const std::size_t file_size = static_cast<std::size_t>(file_.tellg());
      file_.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
      data_bytes_ = (size == 0 || size == 0xFFFFFFFFu)
                        ? file_size - data_offset_
                        : std::min<std::size_t>(size, file_size - data_offset_);
      if (audio_format != 1) return AvpErr::kErrNotSupported;
      return AvpErr::kOk;
    } else {
      file_.seekg(static_cast<std::streamoff>(size + (size & 1)), std::ios::cur);
    }
  }
  return AvpErr::kErrFail;
}

AvpErr FileAudioSource::Start() {
  if (!file_.is_open()) return AvpErr::kErrNotInit;
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
  samples_emitted_ = 0;
  frames_emitted_ = 0;
  eof_ = false;
  paced_ = false;
  return AvpErr::kOk;
}

void FileAudioSource::Stop() {
  if (file_.is_open()) file_.clear();
}

bool FileAudioSource::FillFrame() {
  const std::size_t want_bytes = frame_values_ * sizeof(int16_t);
  file_.read(reinterpret_cast<char*>(frame_buf_.data()), static_cast<std::streamsize>(want_bytes));
  std::streamsize got = file_.gcount();
  if (got < 0) {
    ++errors_;
    return false;
  }
  if (static_cast<std::size_t>(got) == want_bytes) {
    eof_ = false;
    return true;
  }

  // End of file (or a short final frame). Pad the remainder with silence.
  const std::size_t got_samples = static_cast<std::size_t>(got) / sizeof(int16_t);
  if (got > 0) {
    std::fill(frame_buf_.begin() + got_samples, frame_buf_.end(), 0);
  }

  if (loop_) {
    // A partial frame is padded; the next frame restarts the file.
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
    eof_ = false;
    return got > 0;
  }

  eof_ = true;
  return got > 0;
}

AvpErr FileAudioSource::Read(CaptureFrame& out, uint32_t timeout_ms) {
  out = CaptureFrame{};
  if (!file_.is_open()) return AvpErr::kErrNotInit;
  if (eof_) return AvpErr::kErrTimeout;

  if (realtime_) {
    const auto now = std::chrono::steady_clock::now();
    if (!paced_) {
      next_due_ = now;
      paced_ = true;
    }
    if (now < next_due_) {
      if (timeout_ms == 0) return AvpErr::kErrTimeout;
      std::this_thread::sleep_until(next_due_);
    }
  }

  if (!FillFrame()) return AvpErr::kErrTimeout;

  out.data = reinterpret_cast<const uint8_t*>(frame_buf_.data());
  out.bytes = frame_values_ * sizeof(int16_t);
  out.samples = frame_samples_;
  out.pcm = frame_buf_.data();
  out.pcm_samples = frame_samples_;
  out.format = AudioFormat::kPcmS16le;
  out.timestamp_ms = static_cast<uint32_t>(samples_emitted_ * 1000 /
                                           static_cast<uint64_t>(cfg_.sample_rate));
  samples_emitted_ += frame_samples_;
  ++frames_emitted_;

  if (realtime_) {
    const auto frame_duration = std::chrono::microseconds(
        static_cast<int64_t>(cfg_.frame_ms) * 1000);
    next_due_ += frame_duration;
    const auto now = std::chrono::steady_clock::now();
    if (now - next_due_ > std::chrono::milliseconds(500)) {
      next_due_ = now;  // do not accumulate a huge debt after a stall
    }
  }
  return AvpErr::kOk;
}

SourceStats FileAudioSource::Stats() const {
  SourceStats s;
  s.frames = frames_emitted_;
  s.errors = errors_;
  return s;
}

}  // namespace recorder
