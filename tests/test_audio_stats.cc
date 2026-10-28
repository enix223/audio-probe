#include "cl_test.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "cl_audio_stats.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> Sine(double amplitude_dbfs, double hz, int rate, std::size_t samples,
                          double dc = 0.0, double gain = 1.0) {
  const double amp = std::pow(10.0, amplitude_dbfs / 20.0) * 32767.0 * gain;
  std::vector<int16_t> out(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    double v = amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / rate) + dc;
    v = std::max(-32768.0, std::min(32767.0, v));
    out[i] = static_cast<int16_t>(std::lround(v));
  }
  return out;
}

}  // namespace

TEST(full_scale_sine_levels_match_theory) {
  const auto pcm = Sine(0.0, 1000.0, 16000, 16000);
  const auto levels = recorder::ComputePcmLevels(pcm.data(), pcm.size(), 1);
  ASSERT_TRUE(levels.valid);
  EXPECT_NEAR(levels.rms_dbfs, -3.01, 0.05);
  EXPECT_NEAR(levels.peak_dbfs, 0.0, 0.05);
  EXPECT_NEAR(levels.stats.rms_dbfs, levels.rms_dbfs, 1e-9);
  EXPECT_EQ(levels.stats.peak, 32767);
  // A full-scale sine reaches the 32760 clipping threshold by definition.
  EXPECT_TRUE(levels.stats.clipped);
  EXPECT_TRUE(levels.stats.valid);
}

TEST(minus_six_dbfs_sine_levels_match_theory) {
  const auto pcm = Sine(-6.0, 1000.0, 16000, 16000);
  const auto levels = recorder::ComputePcmLevels(pcm.data(), pcm.size(), 1);
  ASSERT_TRUE(levels.valid);
  EXPECT_NEAR(levels.rms_dbfs, -9.03, 0.05);
  EXPECT_NEAR(levels.peak_dbfs, -6.0, 0.05);
}

TEST(dc_offset_is_measured) {
  const auto pcm = Sine(-6.0, 1000.0, 16000, 16000, 1000.0);
  const auto levels = recorder::ComputePcmLevels(pcm.data(), pcm.size(), 1);
  ASSERT_TRUE(levels.valid);
  EXPECT_NEAR(levels.dc_offset_lsb, 1000.0, 1.0);
  EXPECT_NEAR(levels.stats.dc_offset, 1000, 1);
}

TEST(clipping_is_detected) {
  const auto pcm = Sine(0.0, 1000.0, 16000, 16000, 0.0, 2.0);
  const auto levels = recorder::ComputePcmLevels(pcm.data(), pcm.size(), 1);
  ASSERT_TRUE(levels.valid);
  EXPECT_TRUE(levels.clipped_samples > 0);
  EXPECT_TRUE(levels.stats.clipped);
  EXPECT_TRUE(levels.peak_dbfs <= 0.01);
}

TEST(silence_has_no_level) {
  std::vector<int16_t> pcm(1600, 0);
  const auto levels = recorder::ComputePcmLevels(pcm.data(), pcm.size(), 1);
  ASSERT_TRUE(levels.valid);
  EXPECT_TRUE(levels.rms_dbfs < -200.0);
  EXPECT_TRUE(levels.peak_dbfs < -200.0);
  EXPECT_EQ(levels.stats.rms_dbfs_x100, static_cast<int16_t>(-24000));
}

TEST(stereo_aggregates_all_channels) {
  auto left = Sine(-6.0, 1000.0, 16000, 1600);
  auto right = Sine(-6.0, 1000.0, 16000, 1600);
  std::vector<int16_t> interleaved(left.size() * 2);
  for (std::size_t i = 0; i < left.size(); ++i) {
    interleaved[i * 2] = left[i];
    interleaved[i * 2 + 1] = right[i];
  }
  const auto levels = recorder::ComputePcmLevels(interleaved.data(), left.size(), 2);
  ASSERT_TRUE(levels.valid);
  EXPECT_EQ(levels.total_samples, left.size() * 2);
  EXPECT_NEAR(levels.rms_dbfs, -9.03, 0.05);
}

TEST(energy_vad_separates_speech_from_floor) {
  recorder::EnergyVad vad;
  vad.Configure(6.0, 5);
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(vad.Analyze(-70.0));  // calibration
  }
  const double noise = vad.noise_dbfs();
  EXPECT_TRUE(noise > -90.0);
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(vad.Analyze(noise + 2.0));
  }
  EXPECT_TRUE(vad.Analyze(noise + 20.0));
  EXPECT_TRUE(vad.Score(noise + 20.0) > 20);
  EXPECT_EQ(vad.Score(noise - 10.0), 0);
  EXPECT_FALSE(vad.Analyze(noise - 5.0));
}

TEST(percentile10_is_low_quantile) {
  std::vector<double> values;
  for (int i = 0; i < 100; ++i) values.push_back(static_cast<double>(i));
  EXPECT_NEAR(recorder::Percentile10(values), 10.0, 0.001);
  EXPECT_NEAR(recorder::Percentile10({}), -200.0, 0.001);
}
