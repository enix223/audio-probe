#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "cl_audio_stats.h"
#include "cl_endpoint_tracker.h"
#include "cl_recorder_types.h"

namespace recorder {

// One frame entering the segmenter. `payload` is the wire payload (PCM bytes
// or one Opus packet); `pcm` is the pre-encode PCM when the platform has it.
struct InputFrame {
  const uint8_t* payload = nullptr;
  std::size_t payload_len = 0;
  const int16_t* pcm = nullptr;
  std::size_t pcm_samples = 0;  // per channel
  std::size_t samples = 0;      // per channel of the wire payload
  AudioFormat format = AudioFormat::kPcmS16le;
  uint32_t timestamp_ms = 0;
  int frame_ms = 20;
  bool discontinuity = false;
  bool stats_valid = false;
  FrameStats stats;
};

struct SegmentStartEvent {
  uint32_t segment_id = 0;
  std::string reason;
  uint32_t t_ms = 0;
  uint32_t onset_t_ms = 0;
  int pre_roll_ms = 0;
  std::string label;
  bool has_tone = false;
  ToneMetadata tone;
};

struct SegmentFrameEvent {
  uint32_t segment_id = 0;
  uint32_t seq = 0;
  uint32_t timestamp_ms = 0;
  AudioFormat format = AudioFormat::kPcmS16le;
  uint16_t samples = 0;
  uint8_t flags = 0;
  FrameStats stats;
  const uint8_t* payload = nullptr;
  std::size_t payload_len = 0;
};

struct SegmentEndEvent {
  uint32_t segment_id = 0;
  std::string reason;
  uint32_t t_ms = 0;
  int frames = 0;
  int frames_dropped = 0;
  int pre_roll_frames = 0;
  int duration_ms = 0;
  int speech_ms = 0;
  double rms_dbfs = -200.0;
  double peak_dbfs = -200.0;
  double clip_pct = 0.0;
  double noise_dbfs = -200.0;
  int tx_drops = 0;
};

class SegmentSink {
 public:
  virtual ~SegmentSink() = default;
  virtual void OnSegmentStart(const SegmentStartEvent& ev) = 0;
  virtual void OnSegmentFrame(const SegmentFrameEvent& ev) = 0;
  virtual void OnSegmentEnd(const SegmentEndEvent& ev) = 0;
};

// Recorder segmenter: device VAD + pre-roll, continuous block splitting, and
// manual test-tone segments. It never encodes and never touches the socket.
class RecorderSegmenter {
 public:
  RecorderSegmenter();

  void SetSink(SegmentSink* sink) { sink_ = sink; }

  void Configure(const AudioConfig& audio, const CaptureConfig& capture,
                 const std::string& label, bool gain_supported, int gain_db);

  void BeginCapture();
  // Ends the capture, closing any open segment. `reason` is a protocol
  // segment_end reason (stop_capture, transport_lost, ...).
  void EndCapture(const std::string& reason);

  void BeginManual(const std::string& label, const ToneMetadata& tone);
  void EndManual(const std::string& reason);

  void Push(const InputFrame& frame);

  bool TakeCaptureComplete();

  bool in_segment() const { return open_; }
  uint32_t current_segment_id() const { return open_ ? current_id_ : last_id_; }
  VadEngine effective_engine() const { return effective_engine_; }
  int segments_produced() const { return segments_produced_; }
  int segments_discarded() const { return segments_discarded_; }

  double last_rms_dbfs() const { return last_rms_dbfs_; }
  double last_peak_dbfs() const { return last_peak_dbfs_; }
  double noise_floor_dbfs() const { return vad_.noise_dbfs(); }
  uint8_t last_vad_score() const { return last_vad_score_; }

 private:
  struct Buffered {
    std::vector<uint8_t> payload;
    std::size_t samples = 0;
    std::size_t pcm_samples = 0;
    int frame_ms = 20;
    uint32_t timestamp_ms = 0;
    bool speech = false;
    bool discontinuity = false;
    bool stats_valid = false;
    FrameStats stats;
    uint64_t capture_index = 0;
  };

  struct Aggregate {
    uint64_t pcm_sample_count = 0;
    double sum_sq = 0.0;
    double peak_linear = 0.0;
    uint64_t clipped = 0;
    int frames = 0;
    int speech_ms = 0;
    int pre_roll_frames = 0;
    int duration_ms = 0;
    std::vector<double> silence_rms_db;

    void Reset() {
      pcm_sample_count = 0;
      sum_sq = 0.0;
      peak_linear = 0.0;
      clipped = 0;
      frames = 0;
      speech_ms = 0;
      pre_roll_frames = 0;
      duration_ms = 0;
      silence_rms_db.clear();
    }
  };

  void ResetCaptureState();
  void OpenSegment(const std::string& reason, uint32_t t_ms, uint32_t onset_t_ms,
                   int pre_roll_ms, const std::string& label, bool has_tone,
                   const ToneMetadata& tone);
  void EmitFrame(const InputFrame& frame, uint32_t segment_id, uint32_t seq,
                 bool pre_roll, bool speech);
  void EmitBuffered(const Buffered& b, uint32_t segment_id, uint32_t seq,
                    bool pre_roll, bool speech);
  void Accumulate(const InputFrame& frame, bool speech);
  void AccumulateBuffered(const Buffered& b, bool speech);
  void CloseSegment(const std::string& reason);
  void OpenFromRing();
  void ResetVadTracking();
  uint32_t FirstRingTimestamp() const;

  SegmentSink* sink_ = nullptr;

  AudioConfig audio_;
  CaptureConfig capture_;
  std::string label_;
  bool gain_supported_ = false;
  int gain_db_ = 0;
  VadEngine effective_engine_ = VadEngine::kEnergy;

  EnergyVad vad_;
  EndpointTracker tracker_;
  std::deque<Buffered> ring_;
  std::size_t ring_capacity_ = 128;
  uint64_t next_capture_index_ = 0;
  int guard_remaining_ms_ = 0;
  int speech_run_ms_ = 0;
  uint64_t speech_run_start_index_ = 0;
  bool have_confirmed_onset_ = false;
  uint64_t confirmed_onset_index_ = 0;
  bool prev_speech_seen_ = false;

  bool open_ = false;
  uint32_t current_id_ = 0;
  uint32_t last_id_ = 0;
  uint32_t next_segment_id_ = 1;
  uint32_t segment_t0_ms_ = 0;
  uint32_t segment_onset_ms_ = 0;
  uint32_t last_frame_end_ms_ = 0;
  uint32_t segment_seq_ = 0;
  std::string segment_reason_;
  std::string segment_label_;
  bool segment_has_tone_ = false;
  ToneMetadata segment_tone_;
  Aggregate agg_;

  bool pending_open_ = false;
  bool reopen_pending_ = false;
  bool capture_complete_ = false;
  bool manual_ = false;
  bool pending_manual_ = false;
  std::string manual_label_;
  ToneMetadata manual_tone_;

  int segments_produced_ = 0;
  int segments_discarded_ = 0;
  double last_rms_dbfs_ = -200.0;
  double last_peak_dbfs_ = -200.0;
  uint8_t last_vad_score_ = protocol::kVadScoreUnknown;
};

}  // namespace recorder
