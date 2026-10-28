#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include "cl_recorder_mutex.h"
#include <string>
#include <vector>

#include "cl_recorder_types.h"

namespace recorder {

// One complete outbound record. The queue never splits or merges records:
// one audio record becomes exactly one WebSocket binary message, and one
// control record becomes exactly one text message.
struct TxRecord {
  enum class Kind : uint8_t { kControl, kAudio };

  Kind kind = Kind::kControl;

  // Control records carry UTF-8 JSON.
  std::string control;

  // Audio records carry one PCM frame or one Opus packet.
  std::vector<uint8_t> payload;
  AudioFormat format = AudioFormat::kPcmS16le;
  uint32_t segment_id = 0;
  uint32_t seq = 0;
  uint32_t timestamp_ms = 0;
  uint16_t samples = 0;
  uint16_t frame_ms = 0;
  uint8_t flags = 0;
  int16_t rms_dbfs_x100 = protocol::kStatUnavailableI16;
  uint16_t peak = protocol::kStatUnavailableU16;
  int16_t dc_offset = protocol::kStatUnavailableI16;
  uint8_t vad_score = protocol::kVadScoreUnknown;

  std::size_t WireBytes() const {
    return kind == Kind::kControl ? control.size() : protocol::kAudioHeaderSize + payload.size();
  }
};

struct TxQueueStats {
  uint64_t audio_enqueued = 0;
  uint64_t audio_dropped = 0;
  uint64_t control_enqueued = 0;
  uint64_t control_rejected = 0;
  uint64_t high_water_records = 0;
};

// Bounded, frame-preserving outbound queue.
//
// Policy (docs/protocol.md §12):
//   - Control records are never dropped while the link is alive. If a control
//     record cannot fit, PushControl returns false and the caller must end the
//     capture with an error.
//   - Audio records may be dropped oldest-first under pressure. The next audio
//     record popped after a drop is marked DISCONTINUITY.
//   - Ordering is FIFO across both kinds; a control record never jumps over
//     audio that precedes it (segment_start must precede its frames).
class TxQueue {
 public:
  struct Config {
    std::size_t max_records = 512;
    std::size_t max_bytes = 1u << 20;
  };

  TxQueue();
  explicit TxQueue(const Config& cfg);

  // Returns false when the control record could not be queued (even after
  // dropping every audio record).
  bool PushControl(TxRecord rec);

  // Returns true when the new audio record was queued. False means it was
  // dropped immediately (the queue was full of control records).
  bool PushAudio(TxRecord rec);

  // Pops the oldest record. Returns false when empty.
  bool Pop(TxRecord* out);

  // Returns a record to the front after a failed transport send. Does not
  // count as a new enqueue and never drops control records.
  void RequeueFront(TxRecord rec);

  // Drop all queued audio (used on reconnect/stop); controls are preserved.
  void ClearAudio();
  void ClearAll();
  void ResetStats();

  std::size_t Level() const;
  std::size_t Bytes() const;
  int AudioLevelMs() const;
  TxQueueStats Stats() const;

 private:
  bool HasRoomFor(std::size_t bytes_added) const;
  bool DropOldestAudio();
  void AddRecord(TxRecord&& rec);

  mutable RecorderMutex mu_;
  std::deque<TxRecord> q_;
  std::size_t bytes_ = 0;
  int audio_ms_ = 0;
  bool discontinuity_pending_ = false;
  TxQueueStats stats_;
  Config cfg_;
};

}  // namespace recorder
