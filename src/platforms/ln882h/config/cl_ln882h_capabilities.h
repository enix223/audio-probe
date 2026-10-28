#pragma once

#include "cl_recorder_types.h"

namespace recorder {

// LN882H + GX8006 capability descriptor.
//
// The first milestone advertises Opus-only and no device pre-roll: the chip
// mic path is chip-owned Opus and PCM output is unproven. Test tone/speaker is
// omitted until the GX8006 speaker PCM/Opus chunk limits and flow control are
// bench-validated. Do not add PCM here without the bench test in
// docs/ln882h.md §2.1.
inline Capabilities BuildLn882hCapabilities() {
  Capabilities c;
  c.opus = true;
  c.opus_cap.sample_rates = {16000};
  c.opus_cap.channels = {1};
  c.opus_cap.frame_ms = {20, 40, 60};
  c.opus_cap.bitrate = {6000, 64000};
  c.opus_cap.payload_max = static_cast<int>(kMaxOpusPayload);
  c.opus_cap.encoders = {"gx8006"};

  c.vad = {"server", "device"};
  c.gain_supported = true;
  c.gain_min = 0;
  c.gain_max = 42;
  c.gain_step = 6;
  c.gain_unit = "dB";

  c.test_tone = false;
  c.speaker = false;
  c.loopback = false;
  c.device_pre_roll_max_ms = 0;
  c.server_pre_roll = true;
  c.max_segment_ms = 30000;
  c.tx_queue_ms = 3000;
  c.max_frame_bytes = 2048;

#if defined(CL_GX8006_PCM_PROVEN)
  // Enable only after the bench test proves GET_MIC_DATA is PCM and records
  // the measured frame size/rate. The values below are the expected ones, not
  // verified facts.
  c.pcm_s16le = true;
  c.pcm.sample_rates = {16000};
  c.pcm.channels = {1};
  c.pcm.frame_ms = {20};
  c.pcm.payload_max = 640;
  c.pcm.channel_map = {"mic0"};
  c.device_pre_roll_max_ms = 2000;
#endif
  return c;
}

}  // namespace recorder
