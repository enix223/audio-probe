#include "cl_recorder_segmenter.h"

#include <algorithm>
#include <cmath>

namespace recorder {

namespace {
constexpr int kRingMarginMs = 250;
constexpr std::size_t kMaxRingFrames = 8192;
}  // namespace

RecorderSegmenter::RecorderSegmenter() = default;

void RecorderSegmenter::Configure(const AudioConfig& audio, const CaptureConfig& capture,
                                  const std::string& label, bool gain_supported, int gain_db) {
  audio_ = audio;
  capture_ = capture;
  label_ = label;
  gain_supported_ = gain_supported;
  gain_db_ = gain_db;

  // A device-side energy VAD needs PCM. Host-encoded Opus has it; chip-owned
  // Opus does not, and must use the server engine.
  const bool has_local_pcm =
      audio.format == AudioFormat::kPcmS16le || audio.opus_encoder.empty() ||
      audio.opus_encoder == "host";
  effective_engine_ = capture.vad.engine;
  if (effective_engine_ == VadEngine::kAuto) {
    effective_engine_ = has_local_pcm ? VadEngine::kEnergy : VadEngine::kServer;
  } else if (effective_engine_ == VadEngine::kEnergy && !has_local_pcm) {
    effective_engine_ = VadEngine::kServer;
  } else if (effective_engine_ == VadEngine::kDevice) {
    // The Linux recorder has no chip utterance events. The app rejects this
    // during negotiation; keep the segmenter deterministic if it slips in.
    effective_engine_ = has_local_pcm ? VadEngine::kEnergy : VadEngine::kServer;
  }

  vad_.Configure(capture_.vad.threshold_db, std::max(1, 100 / std::max(1, audio.frame_ms)));

  EndpointTracker::Config tcfg;
  tcfg.onset_ms = static_cast<uint32_t>(std::max(0, capture_.vad.onset_ms));
  tcfg.silence_ms = static_cast<uint32_t>(std::max(0, capture_.vad.silence_ms));
  tcfg.min_speech_ms = static_cast<uint32_t>(std::max(0, capture_.vad.min_speech_ms));
  tracker_.Configure(tcfg);

  const int window_ms = capture_.pre_roll_ms + capture_.vad.onset_ms +
                        capture_.vad.min_speech_ms + kRingMarginMs;
  std::size_t cap = static_cast<std::size_t>(std::max(0, window_ms) / std::max(1, audio.frame_ms)) + 2;
  cap = std::max<std::size_t>(8, std::min(cap, kMaxRingFrames));
  ring_capacity_ = cap;

  ResetCaptureState();
}

void RecorderSegmenter::ResetCaptureState() {
  open_ = false;
  last_id_ = 0;
  ring_.clear();
  agg_.Reset();
  next_capture_index_ = 0;
  capture_complete_ = false;
  pending_open_ = false;
  reopen_pending_ = false;
  manual_ = false;
  pending_manual_ = false;
  prev_speech_seen_ = false;
}

void RecorderSegmenter::ResetVadTracking() {
  speech_run_ms_ = 0;
  have_confirmed_onset_ = false;
  confirmed_onset_index_ = 0;
  prev_speech_seen_ = false;
  tracker_.Reset();
  guard_remaining_ms_ = 0;
}

void RecorderSegmenter::BeginCapture() {
  ResetCaptureState();
  vad_.Reset();
  ResetVadTracking();
  guard_remaining_ms_ = std::max(0, capture_.vad.guard_ms);
  last_rms_dbfs_ = -200.0;
  last_peak_dbfs_ = -200.0;

  // VAD mode opens on the first confirmed utterance. Continuous/single (and
  // any server-engine capture) open on the first frame.
  if (capture_.mode != CaptureMode::kVad || effective_engine_ == VadEngine::kServer) {
    pending_open_ = true;
  }
}

void RecorderSegmenter::OpenSegment(const std::string& reason, uint32_t t_ms, uint32_t onset_t_ms,
                                    int pre_roll_ms, const std::string& label, bool has_tone,
                                    const ToneMetadata& tone) {
  open_ = true;
  current_id_ = next_segment_id_++;
  last_id_ = current_id_;
  segment_t0_ms_ = t_ms;
  segment_onset_ms_ = onset_t_ms;
  segment_seq_ = 0;
  segment_reason_ = reason;
  segment_label_ = label;
  segment_has_tone_ = has_tone;
  segment_tone_ = tone;
  last_frame_end_ms_ = t_ms;
  agg_.Reset();
  ++segments_produced_;

  if (sink_ != nullptr) {
    SegmentStartEvent ev;
    ev.segment_id = current_id_;
    ev.reason = reason;
    ev.t_ms = t_ms;
    ev.onset_t_ms = onset_t_ms;
    ev.pre_roll_ms = pre_roll_ms;
    ev.label = label;
    ev.has_tone = has_tone;
    ev.tone = tone;
    sink_->OnSegmentStart(ev);
  }
}

void RecorderSegmenter::EmitFrame(const InputFrame& frame, uint32_t segment_id, uint32_t seq,
                                  bool pre_roll, bool speech) {
  if (sink_ == nullptr) return;
  SegmentFrameEvent ev;
  ev.segment_id = segment_id;
  ev.seq = seq;
  ev.timestamp_ms = frame.timestamp_ms;
  ev.format = frame.format;
  ev.samples = static_cast<uint16_t>(std::min<std::size_t>(frame.samples, 0xFFFF));
  ev.stats = frame.stats;
  ev.payload = frame.payload;
  ev.payload_len = frame.payload_len;
  uint8_t flags = 0;
  if (pre_roll) flags |= AudioFlag(kFlagPreRoll);
  if (speech) flags |= AudioFlag(kFlagSpeech);
  if (frame.stats.valid) flags |= AudioFlag(kFlagStatsValid);
  if (frame.stats.valid && frame.stats.clipped) flags |= AudioFlag(kFlagClipped);
  if (frame.discontinuity) flags |= AudioFlag(kFlagDiscontinuity);
  ev.flags = flags;
  sink_->OnSegmentFrame(ev);
}

void RecorderSegmenter::EmitBuffered(const Buffered& b, uint32_t segment_id, uint32_t seq,
                                     bool pre_roll, bool speech) {
  if (sink_ == nullptr) return;
  InputFrame f;
  f.payload = b.payload.data();
  f.payload_len = b.payload.size();
  f.samples = b.samples;
  f.format = audio_.format;
  f.timestamp_ms = b.timestamp_ms;
  f.frame_ms = b.frame_ms;
  f.discontinuity = b.discontinuity;
  f.stats_valid = b.stats_valid;
  f.stats = b.stats;
  EmitFrame(f, segment_id, seq, pre_roll, speech);
}

void RecorderSegmenter::Accumulate(const InputFrame& frame, bool speech) {
  if (frame.pcm != nullptr && frame.pcm_samples > 0 && audio_.channels > 0) {
    const PcmLevels levels = ComputePcmLevels(frame.pcm, frame.pcm_samples, audio_.channels);
    if (levels.valid) {
      agg_.pcm_sample_count += levels.total_samples;
      const double rms_linear = std::pow(10.0, levels.rms_dbfs / 20.0);
      agg_.sum_sq += rms_linear * rms_linear * static_cast<double>(levels.total_samples);
      agg_.peak_linear = std::max(agg_.peak_linear, levels.peak_linear);
      agg_.clipped += levels.clipped_samples;
      last_rms_dbfs_ = levels.rms_dbfs;
      last_peak_dbfs_ = levels.peak_dbfs;
      if (!speech && agg_.silence_rms_db.size() < 8192) {
        agg_.silence_rms_db.push_back(levels.rms_dbfs);
      }
    }
  } else if (frame.stats.valid) {
    const double rms_linear = std::pow(10.0, frame.stats.rms_dbfs / 20.0);
    const uint64_t n = static_cast<uint64_t>(frame.samples) * static_cast<uint64_t>(audio_.channels);
    agg_.pcm_sample_count += n;
    agg_.sum_sq += rms_linear * rms_linear * static_cast<double>(n);
    agg_.peak_linear = std::max(agg_.peak_linear, static_cast<double>(frame.stats.peak) / 32768.0);
    if (frame.stats.clipped) ++agg_.clipped;
    last_rms_dbfs_ = frame.stats.rms_dbfs;
    last_peak_dbfs_ = LinearToDbfs(static_cast<double>(frame.stats.peak) / 32768.0);
    if (!speech && agg_.silence_rms_db.size() < 8192) {
      agg_.silence_rms_db.push_back(frame.stats.rms_dbfs);
    }
  }
  ++agg_.frames;
  agg_.duration_ms += frame.frame_ms;
  if (speech) agg_.speech_ms += frame.frame_ms;
  last_frame_end_ms_ = frame.timestamp_ms + static_cast<uint32_t>(frame.frame_ms);
}

void RecorderSegmenter::AccumulateBuffered(const Buffered& b, bool speech) {
  Accumulate([&] {
    InputFrame f;
    f.payload = b.payload.data();
    f.payload_len = b.payload.size();
    f.pcm = nullptr;
    f.pcm_samples = 0;
    f.samples = b.samples;
    f.format = audio_.format;
    f.timestamp_ms = b.timestamp_ms;
    f.frame_ms = b.frame_ms;
    f.stats_valid = b.stats_valid;
    f.stats = b.stats;
    return f;
  }(),
             speech);
}

void RecorderSegmenter::CloseSegment(const std::string& reason) {
  if (!open_) return;

  SegmentEndEvent ev;
  ev.segment_id = current_id_;
  ev.reason = reason;
  ev.t_ms = last_frame_end_ms_;
  ev.frames = agg_.frames;
  ev.pre_roll_frames = agg_.pre_roll_frames;
  ev.duration_ms = agg_.duration_ms;
  ev.speech_ms = agg_.speech_ms;

  if (agg_.pcm_sample_count > 0) {
    const double mean_sq = agg_.sum_sq / static_cast<double>(agg_.pcm_sample_count);
    ev.rms_dbfs = 10.0 * std::log10(mean_sq + 1e-12);
    ev.peak_dbfs = LinearToDbfs(agg_.peak_linear);
    ev.clip_pct = 100.0 * static_cast<double>(agg_.clipped) / static_cast<double>(agg_.pcm_sample_count);
  } else {
    ev.rms_dbfs = -200.0;
    ev.peak_dbfs = -200.0;
    ev.clip_pct = 0.0;
  }
  if (!agg_.silence_rms_db.empty()) {
    ev.noise_dbfs = Percentile10(agg_.silence_rms_db);
  } else {
    ev.noise_dbfs = vad_.noise_dbfs();
  }
  if (sink_ != nullptr) sink_->OnSegmentEnd(ev);

  last_rms_dbfs_ = ev.rms_dbfs;
  last_peak_dbfs_ = ev.peak_dbfs;

  open_ = false;
  agg_.Reset();
  last_id_ = current_id_;
  segment_reason_.clear();
  segment_label_.clear();
  segment_has_tone_ = false;
}

uint32_t RecorderSegmenter::FirstRingTimestamp() const {
  return ring_.empty() ? 0 : ring_.front().timestamp_ms;
}

void RecorderSegmenter::OpenFromRing() {
  if (ring_.empty()) return;

  uint64_t onset_idx = have_confirmed_onset_ ? confirmed_onset_index_ : ring_.front().capture_index;
  const int frame_ms = std::max(1, audio_.frame_ms);
  const int pre_roll_frames = (std::max(0, capture_.pre_roll_ms) + frame_ms - 1) / frame_ms;
  const uint64_t start_idx = onset_idx > static_cast<uint64_t>(pre_roll_frames)
                                 ? onset_idx - static_cast<uint64_t>(pre_roll_frames)
                                 : 0;

  auto first = ring_.begin();
  while (first != ring_.end() && first->capture_index < start_idx) ++first;
  if (first == ring_.end()) first = std::prev(ring_.end());

  auto onset_it = first;
  while (onset_it != ring_.end() && onset_it->capture_index < onset_idx) ++onset_it;
  if (onset_it == ring_.end()) onset_it = std::prev(ring_.end());

  const uint32_t t_ms = first->timestamp_ms;
  const uint32_t onset_ms = onset_it->timestamp_ms;
  const int actual_pre_roll_ms = static_cast<int>(onset_ms - t_ms);
  const uint64_t onset_capture_index = onset_it->capture_index;

  OpenSegment("vad", t_ms, onset_ms, actual_pre_roll_ms, label_, false, ToneMetadata{});

  for (auto it = first; it != ring_.end(); ++it) {
    const bool is_pre_roll = it->capture_index < onset_capture_index;
    EmitBuffered(*it, current_id_, segment_seq_++, is_pre_roll, it->speech);
    AccumulateBuffered(*it, it->speech);
    if (is_pre_roll) ++agg_.pre_roll_frames;
  }
  ring_.clear();
}

void RecorderSegmenter::Push(const InputFrame& frame) {
  if (frame.payload == nullptr || frame.payload_len == 0) return;

  InputFrame f = frame;
  bool speech = false;
  if (f.pcm != nullptr && f.pcm_samples > 0 && effective_engine_ != VadEngine::kServer) {
    const PcmLevels levels = ComputePcmLevels(f.pcm, f.pcm_samples, audio_.channels);
    if (levels.valid) {
      f.stats = levels.stats;
      f.stats_valid = true;
      speech = vad_.Analyze(levels.rms_dbfs);
      f.stats.vad_score = vad_.Score(levels.rms_dbfs);
    }
  }
  if (!f.stats_valid) f.stats.vad_score = protocol::kVadScoreUnknown;
  last_vad_score_ = f.stats.vad_score;

  // Guard: frames are still forwarded, but their VAD decision is suppressed.
  if (guard_remaining_ms_ > 0) {
    guard_remaining_ms_ = std::max(0, guard_remaining_ms_ - f.frame_ms);
    speech = false;
  }

  if (manual_) {
    if (!open_ && pending_manual_) {
      OpenSegment("test_tone", f.timestamp_ms, f.timestamp_ms, 0, manual_label_, true, manual_tone_);
      pending_manual_ = false;
    }
    if (open_) {
      EmitFrame(f, current_id_, segment_seq_++, false, false);
      Accumulate(f, false);
    }
    return;
  }

  const bool segmentation_vad =
      capture_.mode == CaptureMode::kVad && effective_engine_ != VadEngine::kServer;

  if (!segmentation_vad) {
    // Continuous/single/server-VAD: forward every frame, split at the block
    // boundary, and report VAD decisions as telemetry only.
    if (!open_ && pending_open_) {
      const bool single = capture_.mode == CaptureMode::kSingle;
      OpenSegment(single ? "single" : "continuous", f.timestamp_ms, f.timestamp_ms, 0,
                  label_, false, ToneMetadata{});
      pending_open_ = false;
    }
    if (!open_) return;

    EmitFrame(f, current_id_, segment_seq_++, false, speech);
    Accumulate(f, speech);

    int limit_ms = capture_.max_segment_ms;
    if (capture_.mode == CaptureMode::kContinuous) {
      limit_ms = capture_.segment_ms > 0 ? capture_.segment_ms : limit_ms;
    } else if (capture_.mode == CaptureMode::kSingle) {
      limit_ms = capture_.duration_ms > 0 ? capture_.duration_ms : limit_ms;
    }
    if (limit_ms > 0 && agg_.duration_ms >= limit_ms) {
      CloseSegment("max_duration");
      if (capture_.mode == CaptureMode::kSingle) {
        capture_complete_ = true;
      } else {
        pending_open_ = true;
      }
    }
    return;
  }

  // ---- device VAD mode ----------------------------------------------------
  if (open_) {
    EmitFrame(f, current_id_, segment_seq_++, false, speech);
    Accumulate(f, speech);

    const bool ended = tracker_.Feed(speech, static_cast<uint32_t>(std::max(1, f.frame_ms)));
    if (ended) {
      CloseSegment("silence");
      ResetVadTracking();
      ring_.clear();
      return;
    }
    if (capture_.max_segment_ms > 0 && agg_.duration_ms >= capture_.max_segment_ms) {
      CloseSegment("max_duration");
      // Keep the tracker state: if speech continues, the next frame reopens
      // without a pre-roll. Silence will disarm it on the next Feed.
      reopen_pending_ = speech;
    }
    return;
  }

  // Reopen immediately after a max-duration split while speech continues.
  if (reopen_pending_ && speech) {
    OpenSegment("vad", f.timestamp_ms, f.timestamp_ms, 0, label_, false, ToneMetadata{});
    reopen_pending_ = false;
    EmitFrame(f, current_id_, segment_seq_++, false, true);
    Accumulate(f, true);
    tracker_.Feed(true, static_cast<uint32_t>(std::max(1, f.frame_ms)));
    return;
  }
  if (!speech) reopen_pending_ = false;

  // Buffer for pre-roll while waiting for a confirmed onset.
  Buffered b;
  b.payload.assign(f.payload, f.payload + f.payload_len);
  b.samples = f.samples;
  b.pcm_samples = f.pcm_samples;
  b.frame_ms = f.frame_ms;
  b.timestamp_ms = f.timestamp_ms;
  b.speech = speech;
  b.discontinuity = f.discontinuity;
  b.stats_valid = f.stats_valid;
  b.stats = f.stats;
  b.capture_index = next_capture_index_++;
  ring_.push_back(std::move(b));
  while (ring_.size() > ring_capacity_) ring_.pop_front();

  if (speech) {
    if (speech_run_ms_ == 0) speech_run_start_index_ = ring_.back().capture_index;
    speech_run_ms_ += std::max(1, f.frame_ms);
    if (speech_run_ms_ >= capture_.vad.onset_ms) {
      have_confirmed_onset_ = true;
      confirmed_onset_index_ = speech_run_start_index_;
    }
  } else {
    speech_run_ms_ = 0;
    have_confirmed_onset_ = false;
  }

  const bool was_seen = prev_speech_seen_;
  tracker_.Feed(speech, static_cast<uint32_t>(std::max(1, f.frame_ms)));
  const bool now_seen = tracker_.SpeechSeen();
  prev_speech_seen_ = now_seen;
  if (!was_seen && now_seen) {
    OpenFromRing();
  }
}

void RecorderSegmenter::EndCapture(const std::string& reason) {
  if (open_) {
    CloseSegment(reason);
  } else if (manual_ && pending_manual_) {
    // Nothing was emitted.
  } else if (capture_.mode == CaptureMode::kVad && speech_run_ms_ > 0) {
    // Speech was accumulating but never reached min_speech_ms. Count it as a
    // discarded short segment; send_short_segments is only meaningful once a
    // segment has opened.
    ++segments_discarded_;
  }
  ring_.clear();
  ResetVadTracking();
  manual_ = false;
  pending_manual_ = false;
  pending_open_ = false;
  reopen_pending_ = false;
}

void RecorderSegmenter::BeginManual(const std::string& label, const ToneMetadata& tone) {
  if (open_) CloseSegment("stop_capture");
  manual_ = true;
  pending_manual_ = true;
  manual_label_ = label;
  manual_tone_ = tone;
  ring_.clear();
  ResetVadTracking();
}

void RecorderSegmenter::EndManual(const std::string& reason) {
  if (open_) CloseSegment(reason);
  manual_ = false;
  pending_manual_ = false;
}

bool RecorderSegmenter::TakeCaptureComplete() {
  const bool v = capture_complete_;
  capture_complete_ = false;
  return v;
}

}  // namespace recorder
