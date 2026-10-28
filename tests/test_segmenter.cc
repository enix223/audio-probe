#include "cl_test.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "cl_recorder_segmenter.h"

namespace {

constexpr int kRate = 16000;
constexpr int kFrameMs = 20;
constexpr std::size_t kFrameSamples = kRate * kFrameMs / 1000;

std::vector<int16_t> Silence() { return std::vector<int16_t>(kFrameSamples, 0); }

std::vector<int16_t> Speech(double dbfs = -20.0, std::size_t frame_index = 0) {
  const double amp = std::pow(10.0, dbfs / 20.0) * 32768.0;
  std::vector<int16_t> out(kFrameSamples);
  for (std::size_t i = 0; i < kFrameSamples; ++i) {
    const double t = static_cast<double>(frame_index * kFrameSamples + i) / kRate;
    out[i] = static_cast<int16_t>(std::lround(amp * std::sin(2.0 * 3.141592653589793 * 220.0 * t)));
  }
  return out;
}

struct Sink : public recorder::SegmentSink {
  struct Start {
    uint32_t id;
    std::string reason;
    uint32_t t_ms;
    uint32_t onset_ms;
    int pre_roll_ms;
    std::string label;
  };
  struct Frame {
    uint32_t id;
    uint32_t seq;
    uint32_t ts;
    uint8_t flags;
    std::size_t bytes;
  };
  struct End {
    uint32_t id;
    std::string reason;
    uint32_t t_ms;
    int frames;
    int pre_roll_frames;
    int duration_ms;
    int speech_ms;
  };

  std::vector<Start> starts;
  std::vector<Frame> frames;
  std::vector<End> ends;

  void OnSegmentStart(const recorder::SegmentStartEvent& ev) override {
    starts.push_back(Start{ev.segment_id, ev.reason, ev.t_ms, ev.onset_t_ms, ev.pre_roll_ms, ev.label});
  }
  void OnSegmentFrame(const recorder::SegmentFrameEvent& ev) override {
    frames.push_back(Frame{ev.segment_id, ev.seq, ev.timestamp_ms, ev.flags, ev.payload_len});
  }
  void OnSegmentEnd(const recorder::SegmentEndEvent& ev) override {
    ends.push_back(End{ev.segment_id, ev.reason, ev.t_ms, ev.frames, ev.pre_roll_frames,
                       ev.duration_ms, ev.speech_ms});
  }
};

recorder::InputFrame MakeFrame(const std::vector<int16_t>& pcm, uint32_t index) {
  recorder::InputFrame f;
  f.payload = reinterpret_cast<const uint8_t*>(pcm.data());
  f.payload_len = pcm.size() * sizeof(int16_t);
  f.pcm = pcm.data();
  f.pcm_samples = pcm.size();
  f.samples = pcm.size();
  f.format = recorder::AudioFormat::kPcmS16le;
  f.timestamp_ms = index * kFrameMs;
  f.frame_ms = kFrameMs;
  return f;
}

recorder::AudioConfig Pcm16() {
  recorder::AudioConfig a;
  a.format = recorder::AudioFormat::kPcmS16le;
  a.sample_rate = kRate;
  a.channels = 1;
  a.frame_ms = kFrameMs;
  return a;
}

recorder::CaptureConfig VadCapture() {
  recorder::CaptureConfig c;
  c.mode = recorder::CaptureMode::kVad;
  c.pre_roll_ms = 300;
  c.max_segment_ms = 15000;
  c.vad.engine = recorder::VadEngine::kAuto;
  c.vad.onset_ms = 120;
  c.vad.silence_ms = 700;
  c.vad.min_speech_ms = 150;
  c.vad.threshold_db = 6.0;
  return c;
}

}  // namespace

TEST(segmenter_vad_pre_roll_and_silence_end) {
  recorder::RecorderSegmenter seg;
  Sink sink;
  seg.SetSink(&sink);
  seg.Configure(Pcm16(), VadCapture(), "unit", false, 0);
  seg.BeginCapture();

  uint32_t index = 0;
  for (int i = 0; i < 50; ++i) seg.Push(MakeFrame(Silence(), index++));
  for (int i = 0; i < 50; ++i) seg.Push(MakeFrame(Speech(-20.0, index), index++));
  for (int i = 0; i < 50; ++i) seg.Push(MakeFrame(Silence(), index++));
  seg.EndCapture("stop_capture");

  ASSERT_TRUE(sink.starts.size() == 1);
  ASSERT_TRUE(sink.ends.size() == 1);
  const auto& start = sink.starts[0];
  const auto& end = sink.ends[0];
  EXPECT_EQ(start.reason, std::string("vad"));
  EXPECT_EQ(start.t_ms, 700u);
  EXPECT_EQ(start.onset_ms, 1000u);
  EXPECT_EQ(start.pre_roll_ms, 300);
  EXPECT_EQ(end.reason, std::string("silence"));
  EXPECT_EQ(end.frames, 100);
  EXPECT_EQ(end.pre_roll_frames, 15);
  EXPECT_EQ(end.duration_ms, 2000);
  EXPECT_EQ(end.speech_ms, 1000);

  // Sequence numbers are 0..99, unique and monotonic; pre-roll frames are
  // flagged exactly once and precede live frames.
  ASSERT_TRUE(sink.frames.size() == 100);
  int pre_roll = 0;
  for (std::size_t i = 0; i < sink.frames.size(); ++i) {
    EXPECT_EQ(sink.frames[i].seq, static_cast<uint32_t>(i));
    if (sink.frames[i].flags & recorder::AudioFlag(recorder::kFlagPreRoll)) {
      ++pre_roll;
      EXPECT_TRUE(i < 15);
    }
    EXPECT_TRUE((sink.frames[i].flags & recorder::AudioFlag(recorder::kFlagStatsValid)) != 0);
  }
  EXPECT_EQ(pre_roll, 15);
}

TEST(segmenter_short_blip_does_not_open) {
  recorder::RecorderSegmenter seg;
  Sink sink;
  seg.SetSink(&sink);
  seg.Configure(Pcm16(), VadCapture(), "unit", false, 0);
  seg.BeginCapture();

  uint32_t index = 0;
  for (int i = 0; i < 30; ++i) seg.Push(MakeFrame(Silence(), index++));
  for (int i = 0; i < 3; ++i) seg.Push(MakeFrame(Speech(-20.0, index), index++));
  for (int i = 0; i < 30; ++i) seg.Push(MakeFrame(Silence(), index++));
  seg.EndCapture("stop_capture");

  EXPECT_TRUE(sink.starts.empty());
  EXPECT_TRUE(sink.ends.empty());
  EXPECT_EQ(seg.segments_produced(), 0);
}

TEST(segmenter_stop_during_unconfirmed_speech_counts_discard) {
  recorder::RecorderSegmenter seg;
  Sink sink;
  seg.SetSink(&sink);
  seg.Configure(Pcm16(), VadCapture(), "unit", false, 0);
  seg.BeginCapture();

  uint32_t index = 0;
  for (int i = 0; i < 20; ++i) seg.Push(MakeFrame(Silence(), index++));
  for (int i = 0; i < 5; ++i) seg.Push(MakeFrame(Speech(-20.0, index), index++));
  seg.EndCapture("stop_capture");

  EXPECT_TRUE(sink.starts.empty());
  EXPECT_EQ(seg.segments_discarded(), 1);
}

TEST(segmenter_continuous_splits_at_segment_ms) {
  recorder::RecorderSegmenter seg;
  Sink sink;
  seg.SetSink(&sink);
  recorder::CaptureConfig c;
  c.mode = recorder::CaptureMode::kContinuous;
  c.segment_ms = 200;
  c.max_segment_ms = 1000;
  c.vad.engine = recorder::VadEngine::kServer;
  seg.Configure(Pcm16(), c, "cont", false, 0);
  seg.BeginCapture();

  for (uint32_t i = 0; i < 30; ++i) seg.Push(MakeFrame(Silence(), i));
  seg.EndCapture("stop_capture");

  ASSERT_TRUE(sink.starts.size() == 3);
  ASSERT_TRUE(sink.ends.size() == 3);
  EXPECT_EQ(sink.starts[0].reason, std::string("continuous"));
  EXPECT_EQ(sink.ends[0].reason, std::string("max_duration"));
  EXPECT_EQ(sink.ends[0].frames, 10);
  EXPECT_EQ(sink.ends[0].duration_ms, 200);
  EXPECT_EQ(sink.ends[1].frames, 10);
  EXPECT_EQ(sink.ends[1].duration_ms, 200);
  EXPECT_EQ(sink.starts[1].t_ms, 200u);
  EXPECT_EQ(sink.ends[2].reason, std::string("max_duration"));
  EXPECT_EQ(sink.ends[2].frames, 10);
}

TEST(segmenter_single_mode_completes) {
  recorder::RecorderSegmenter seg;
  Sink sink;
  seg.SetSink(&sink);
  recorder::CaptureConfig c;
  c.mode = recorder::CaptureMode::kSingle;
  c.duration_ms = 100;
  c.segment_ms = 1000;
  c.max_segment_ms = 1000;
  c.vad.engine = recorder::VadEngine::kServer;
  seg.Configure(Pcm16(), c, "single", false, 0);
  seg.BeginCapture();

  for (uint32_t i = 0; i < 10; ++i) seg.Push(MakeFrame(Silence(), i));
  EXPECT_TRUE(seg.TakeCaptureComplete());
  seg.EndCapture("stop_capture");

  ASSERT_TRUE(sink.starts.size() == 1);
  ASSERT_TRUE(sink.ends.size() == 1);
  EXPECT_EQ(sink.starts[0].reason, std::string("single"));
  EXPECT_EQ(sink.ends[0].reason, std::string("max_duration"));
  EXPECT_EQ(sink.ends[0].frames, 5);
}

TEST(segmenter_server_vad_mode_does_not_gate) {
  recorder::RecorderSegmenter seg;
  Sink sink;
  seg.SetSink(&sink);
  recorder::CaptureConfig c;
  c.mode = recorder::CaptureMode::kContinuous;
  c.segment_ms = 1000;
  c.max_segment_ms = 1000;
  c.vad.engine = recorder::VadEngine::kServer;
  seg.Configure(Pcm16(), c, "server", false, 0);
  seg.BeginCapture();

  for (uint32_t i = 0; i < 5; ++i) seg.Push(MakeFrame(Silence(), i));
  ASSERT_TRUE(sink.starts.size() == 1);
  EXPECT_EQ(sink.starts[0].t_ms, 0u);
  EXPECT_EQ(sink.frames.size(), 5u);
  EXPECT_TRUE(seg.effective_engine() == recorder::VadEngine::kServer);
}
