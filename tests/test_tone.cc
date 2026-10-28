#include "cl_test.h"

#include <cmath>
#include <vector>

#include "cl_audio_stats.h"
#include "cl_recorder_tone.h"

namespace {

recorder::ToneMetadata Tone(const char* waveform, double hz, double dbfs, int duration_ms,
                            int fade_ms = 0) {
  recorder::ToneMetadata t;
  t.tone_id = "test";
  t.waveform = waveform;
  t.frequency_hz = hz;
  t.amplitude_dbfs = dbfs;
  t.duration_ms = duration_ms;
  t.fade_ms = fade_ms;
  return t;
}

std::vector<int16_t> RenderAll(recorder::ToneGenerator* g) {
  std::vector<int16_t> all;
  std::vector<int16_t> chunk(160);
  while (!g->done()) {
    const std::size_t n = g->Render(chunk.data(), chunk.size());
    if (n == 0) break;
    all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<long>(n));
  }
  return all;
}

}  // namespace

TEST(tone_sine_amplitude_and_frequency) {
  recorder::ToneGenerator g;
  ASSERT_TRUE(g.Init(Tone("sine", 1000, -6, 100), 16000, 1) == recorder::AvpErr::kOk);
  const auto pcm = RenderAll(&g);
  EXPECT_EQ(pcm.size(), 1600u);
  const auto levels = recorder::ComputePcmLevels(pcm.data(), pcm.size(), 1);
  EXPECT_NEAR(levels.rms_dbfs, -9.03, 0.05);
  EXPECT_NEAR(levels.peak_dbfs, -6.0, 0.05);

  // Zero-crossing rate for a 1 kHz sine at 16 kHz is ~0.125 per sample pair.
  std::size_t crossings = 0;
  for (std::size_t i = 1; i < pcm.size(); ++i) {
    if ((pcm[i - 1] < 0) != (pcm[i] < 0)) ++crossings;
  }
  const double zcr = static_cast<double>(crossings) / static_cast<double>(pcm.size() - 1);
  EXPECT_NEAR(zcr, 0.125, 0.01);
}

TEST(tone_fade_starts_and_ends_at_zero) {
  recorder::ToneGenerator g;
  ASSERT_TRUE(g.Init(Tone("sine", 1000, -6, 100, 10), 16000, 1) == recorder::AvpErr::kOk);
  const auto pcm = RenderAll(&g);
  ASSERT_TRUE(pcm.size() == 1600);
  EXPECT_TRUE(std::abs(pcm.front()) < 200);
  EXPECT_TRUE(std::abs(pcm.back()) < 200);
  const std::size_t mid = pcm.size() / 2;
  EXPECT_TRUE(std::abs(pcm[mid + 4]) > 1000);
}

TEST(tone_square_has_expected_level) {
  recorder::ToneGenerator g;
  ASSERT_TRUE(g.Init(Tone("square", 1000, -6, 100), 16000, 1) == recorder::AvpErr::kOk);
  const auto pcm = RenderAll(&g);
  const auto levels = recorder::ComputePcmLevels(pcm.data(), pcm.size(), 1);
  EXPECT_NEAR(levels.rms_dbfs, -6.02, 0.1);
  EXPECT_NEAR(levels.peak_dbfs, -6.0, 0.05);
}

TEST(tone_sweep_renders_all_frames) {
  recorder::ToneMetadata t;
  t.tone_id = "sweep";
  t.waveform = "sweep";
  t.start_hz = 100;
  t.end_hz = 4000;
  t.sweep_mode = "log";
  t.amplitude_dbfs = -12;
  t.duration_ms = 200;
  t.fade_ms = 5;
  recorder::ToneGenerator g;
  ASSERT_TRUE(g.Init(t, 16000, 1) == recorder::AvpErr::kOk);
  const auto pcm = RenderAll(&g);
  EXPECT_EQ(pcm.size(), 3200u);
  EXPECT_TRUE(g.done());
}

TEST(tone_rejects_invalid_parameters) {
  recorder::ToneGenerator g;
  EXPECT_TRUE(g.Init(Tone("sine", 0, -6, 100), 16000, 1) == recorder::AvpErr::kErrInvalidArg);
  recorder::ToneMetadata t;
  t.tone_id = "bad";
  t.waveform = "sweep";
  t.start_hz = 0;
  t.end_hz = 0;
  t.amplitude_dbfs = -6;
  t.duration_ms = 100;
  EXPECT_TRUE(g.Init(t, 16000, 1) == recorder::AvpErr::kErrInvalidArg);
}
