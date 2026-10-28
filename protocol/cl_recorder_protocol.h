#pragma once

// Recorder WebSocket protocol v1 — binary audio packet codec.
//
// This header is the C++ half of the shared wire contract in docs/protocol.md.
// The Go server implements the same layout in server/internal/protocol.
// protocol/golden/ is the arbiter: both sides must decode every vector to the
// same values, byte for byte.
//
// Layout (network byte order, big-endian, 28 bytes):
//
//   offset size field
//   0      1    version          (1)
//   1      1    type             (1 = audio)
//   2      1    flags            (see AudioFlags)
//   3      1    format           (1 = PCM_S16LE, 2 = OPUS)
//   4      4    segment_id
//   8      4    seq              (0-based per segment, counts produced frames)
//   12     4    timestamp_ms     (audio clock, relative to capture_t0)
//   16     2    samples          (per channel; TOC-derived for Opus)
//   18     2    payload_len
//   20     2    rms_dbfs_x100    (i16; INT16_MIN = unavailable)
//   22     2    peak             (u16; 0xFFFF = unavailable)
//   24     2    dc_offset        (i16; INT16_MIN = unavailable)
//   26     1    vad_score        (0..100; 255 = unknown)
//   27     1    reserved         (0)

#include <cstddef>
#include <cstdint>

namespace recorder {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kMessageTypeAudio = 1;
constexpr std::size_t kAudioHeaderSize = 28;

// v1 absolute limits from docs/protocol.md §3.3.
constexpr std::size_t kMaxOpusPayload = 255;
constexpr std::size_t kMaxPcmPayload = 4096;
constexpr std::size_t kMaxAudioMessage = kAudioHeaderSize + kMaxPcmPayload;

// Sentinels for unavailable per-frame statistics.
constexpr int16_t kStatUnavailableI16 = INT16_MIN;
constexpr uint16_t kStatUnavailableU16 = 0xFFFF;
constexpr uint8_t kVadScoreUnknown = 255;

enum class AudioFormat : uint8_t {
  kPcmS16le = 1,
  kOpus = 2,
};

enum AudioFlags : uint8_t {
  kFlagPreRoll = 0x01,
  kFlagSpeech = 0x02,
  kFlagClipped = 0x04,
  kFlagDiscontinuity = 0x08,
  kFlagDtxPlc = 0x10,
  kFlagStatsValid = 0x20,
  kFlagReservedMask = 0xC0,
};

inline uint8_t AudioFlag(AudioFlags flag) { return static_cast<uint8_t>(flag); }

struct AudioHeader {
  uint8_t version = kProtocolVersion;
  uint8_t type = kMessageTypeAudio;
  uint8_t flags = 0;
  AudioFormat format = AudioFormat::kPcmS16le;
  uint32_t segment_id = 0;
  uint32_t seq = 0;
  uint32_t timestamp_ms = 0;
  uint16_t samples = 0;
  uint16_t payload_len = 0;
  int16_t rms_dbfs_x100 = kStatUnavailableI16;
  uint16_t peak = kStatUnavailableU16;
  int16_t dc_offset = kStatUnavailableI16;
  uint8_t vad_score = kVadScoreUnknown;
  uint8_t reserved = 0;

  bool HasFlag(AudioFlags flag) const { return (flags & AudioFlag(flag)) != 0; }
  void SetFlag(AudioFlags flag, bool on) {
    if (on) {
      flags |= AudioFlag(flag);
    } else {
      flags &= static_cast<uint8_t>(~AudioFlag(flag));
    }
  }
  bool StatsValid() const { return HasFlag(kFlagStatsValid); }
};

enum class HeaderError : uint8_t {
  kOk = 0,
  kTooShort,           // fewer than 28 bytes
  kBadVersion,         // version != 1
  kBadType,            // type != 1
  kBadFormat,          // format not 1/2
  kReservedBitsSet,    // reserved flag bits or reserved byte non-zero
  kLengthMismatch,     // payload_len != message length - 28
  kPayloadTooLarge,    // 28 + payload_len > max_frame_bytes
  kPcmLengthMismatch,  // payload_len != samples * channels * 2 (needs channels)
  kOpusPayloadTooLarge,// payload_len > 255
  kOpusEmptyPacket,    // zero-length or TOC unparseable
};

// Stable machine-readable code shared with the Go server and the golden
// malformed vectors (the human-readable names live in HeaderErrorName).
inline const char* HeaderErrorCode(HeaderError e) {
  switch (e) {
    case HeaderError::kOk:
      return "ok";
    case HeaderError::kTooShort:
      return "truncated_header";
    case HeaderError::kBadVersion:
      return "unsupported_version";
    case HeaderError::kBadType:
      return "bad_type";
    case HeaderError::kBadFormat:
      return "bad_format";
    case HeaderError::kReservedBitsSet:
      return "reserved_bits";
    case HeaderError::kLengthMismatch:
      return "length_mismatch";
    case HeaderError::kPayloadTooLarge:
      return "payload_too_large";
    case HeaderError::kPcmLengthMismatch:
      return "pcm_length_mismatch";
    case HeaderError::kOpusPayloadTooLarge:
      return "opus_payload_too_large";
    case HeaderError::kOpusEmptyPacket:
      return "opus_empty_packet";
  }
  return "unknown";
}

inline const char* HeaderErrorName(HeaderError e) {
  switch (e) {
    case HeaderError::kOk:
      return "ok";
    case HeaderError::kTooShort:
      return "truncated header";
    case HeaderError::kBadVersion:
      return "unsupported version";
    case HeaderError::kBadType:
      return "not an audio message";
    case HeaderError::kBadFormat:
      return "unknown audio format";
    case HeaderError::kReservedBitsSet:
      return "reserved bits set";
    case HeaderError::kLengthMismatch:
      return "payload length mismatch";
    case HeaderError::kPayloadTooLarge:
      return "payload too large";
    case HeaderError::kPcmLengthMismatch:
      return "pcm payload/sample count mismatch";
    case HeaderError::kOpusPayloadTooLarge:
      return "opus payload too large";
    case HeaderError::kOpusEmptyPacket:
      return "empty or unparseable opus packet";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Big-endian primitives
// ---------------------------------------------------------------------------

inline uint16_t ReadU16Be(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline int16_t ReadI16Be(const uint8_t* p) { return static_cast<int16_t>(ReadU16Be(p)); }

inline uint32_t ReadU32Be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void WriteU16Be(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

inline void WriteU32Be(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

// ---------------------------------------------------------------------------
// Opus TOC validation (RFC 6716 §3.1)
// ---------------------------------------------------------------------------
//
// Returns true when the first one or two bytes describe a structurally
// plausible packet (frame count and duration within the 120 ms cap). It does
// not validate the compressed payload.

struct OpusTocInfo {
  bool ok = false;
  int frame_count = 0;
  int total_us = 0;
};

inline OpusTocInfo ParseOpusToc(const uint8_t* data, std::size_t len) {
  OpusTocInfo info;
  if (data == nullptr || len == 0) return info;
  const uint8_t toc = data[0];

  int spf48;
  if (toc & 0x80) {
    spf48 = 120 << ((toc >> 3) & 0x3);
  } else if ((toc & 0x60) == 0x60) {
    spf48 = (toc & 0x08) ? 960 : 480;
  } else {
    const int k = (toc >> 3) & 0x3;
    spf48 = (k == 3) ? 2880 : (480 << k);
  }

  int count;
  switch (toc & 0x3) {
    case 0:
      count = 1;
      break;
    case 1:
    case 2:
      count = 2;
      break;
    default:
      if (len < 2) return info;
      count = data[1] & 0x3F;
      if (count <= 0) return info;
      break;
  }

  const int samples48 = spf48 * count;
  if (samples48 > 5760) return info;  // RFC 6716 120 ms cap

  info.ok = true;
  info.frame_count = count;
  info.total_us = samples48 * 1000 / 48;
  return info;
}

// ---------------------------------------------------------------------------
// Encode / decode
// ---------------------------------------------------------------------------

inline void EncodeAudioHeader(const AudioHeader& h, uint8_t out[kAudioHeaderSize]) {
  out[0] = h.version;
  out[1] = h.type;
  out[2] = h.flags;
  out[3] = static_cast<uint8_t>(h.format);
  WriteU32Be(out + 4, h.segment_id);
  WriteU32Be(out + 8, h.seq);
  WriteU32Be(out + 12, h.timestamp_ms);
  WriteU16Be(out + 16, h.samples);
  WriteU16Be(out + 18, h.payload_len);
  WriteU16Be(out + 20, static_cast<uint16_t>(h.rms_dbfs_x100));
  WriteU16Be(out + 22, h.peak);
  WriteU16Be(out + 24, static_cast<uint16_t>(h.dc_offset));
  out[26] = h.vad_score;
  out[27] = h.reserved;
}

// Decode a complete WebSocket binary message. `max_frame_bytes` is the
// effective (negotiated) limit; a message larger than it is rejected.
inline HeaderError DecodeAudioHeader(const uint8_t* data, std::size_t len, std::size_t max_frame_bytes,
                                     AudioHeader* out) {
  if (out == nullptr) return HeaderError::kTooShort;
  if (data == nullptr || len < kAudioHeaderSize) return HeaderError::kTooShort;

  AudioHeader h;
  h.version = data[0];
  h.type = data[1];
  h.flags = data[2];
  const uint8_t format = data[3];
  h.segment_id = ReadU32Be(data + 4);
  h.seq = ReadU32Be(data + 8);
  h.timestamp_ms = ReadU32Be(data + 12);
  h.samples = ReadU16Be(data + 16);
  h.payload_len = ReadU16Be(data + 18);
  h.rms_dbfs_x100 = ReadI16Be(data + 20);
  h.peak = ReadU16Be(data + 22);
  h.dc_offset = ReadI16Be(data + 24);
  h.vad_score = data[26];
  h.reserved = data[27];

  if (h.version != kProtocolVersion) return HeaderError::kBadVersion;
  if (h.type != kMessageTypeAudio) return HeaderError::kBadType;
  if (format != static_cast<uint8_t>(AudioFormat::kPcmS16le) &&
      format != static_cast<uint8_t>(AudioFormat::kOpus)) {
    return HeaderError::kBadFormat;
  }
  h.format = static_cast<AudioFormat>(format);

  if ((h.flags & kFlagReservedMask) != 0 || h.reserved != 0) return HeaderError::kReservedBitsSet;

  if (h.payload_len != len - kAudioHeaderSize) return HeaderError::kLengthMismatch;
  if (len > max_frame_bytes) return HeaderError::kPayloadTooLarge;

  if (h.format == AudioFormat::kPcmS16le) {
    if (h.payload_len > kMaxPcmPayload) return HeaderError::kPayloadTooLarge;
  } else {
    if (h.payload_len > kMaxOpusPayload) return HeaderError::kOpusPayloadTooLarge;
    if (h.payload_len == 0) return HeaderError::kOpusEmptyPacket;
    const OpusTocInfo toc = ParseOpusToc(data + kAudioHeaderSize, h.payload_len);
    if (!toc.ok) return HeaderError::kOpusEmptyPacket;
  }

  *out = h;
  return HeaderError::kOk;
}

// PCM payload-length check. The binary header does not carry the channel
// count, so this must be checked against the negotiated configuration.
inline bool ValidatePcmFrame(const AudioHeader& h, int channels) {
  if (h.format != AudioFormat::kPcmS16le || channels <= 0) return false;
  const std::size_t expected = static_cast<std::size_t>(h.samples) * static_cast<std::size_t>(channels) * 2;
  return h.payload_len == expected;
}

inline const char* AudioFormatName(AudioFormat f) {
  switch (f) {
    case AudioFormat::kPcmS16le:
      return "pcm_s16le";
    case AudioFormat::kOpus:
      return "opus";
  }
  return "unknown";
}

}  // namespace recorder

// The recorder's wire contract lives in namespace recorder; this alias keeps
// the protocol vocabulary readable from within that namespace (protocol::k...).
namespace protocol = recorder;
