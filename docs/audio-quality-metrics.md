# Audio Quality Metrics and Diagnostic Procedures

This document defines the measurements the recorder server computes, how they
are calculated, and how to interpret them for two questions:

1. **Is the microphone/module picking up voice correctly?**
2. **How good is the audio chip and its analog path?**

The numbers are only as good as the measurement path. Every report must state
whether the audio was PCM or Opus, whether the capture was acoustic or
loopback, the gain/sample-rate settings, and any packet loss or device drops.

---

## 1. Signal representation and normalization

- Device PCM is signed 16-bit little-endian (`pcm_s16le`) in v1.
- Normalize every sample to `x = sample / 32768`, so `x ∈ [-1, 1)`.
- All dB values are relative to full scale (dBFS):
  `dBFS = 20·log10(amplitude + ε)`, with `ε = 1e-12` to avoid `log(0)`.
- A full-scale sine has RMS `1/√2`, i.e. **−3.01 dBFS**, and peak 0 dBFS.
- Header telemetry from the device is advisory. The server recomputes metrics
  from received PCM (or decoded Opus) and reports device and server values
  separately when they differ.

---

## 2. Frame-level metrics

Frames are the negotiated frame duration (typically 10/20/40/60 ms). The device
computes per-frame stats for the binary header when PCM is available; the
server recomputes them for every frame and aggregates per segment.

| Metric | Formula / definition | Notes |
|---|---|---|
| **RMS** | `20·log10(sqrt(mean(x²)))` dBFS | Over all samples in the frame. |
| **Peak** | `20·log10(max(|x|))` dBFS; also raw `max(|sample|)` | Report both. |
| **Crest factor** | `peak_linear / rms_linear`; dB form `peak_dB − rms_dB` | A high crest factor with low RMS suggests impulsive noise or a click. |
| **DC offset** | `mean(sample)` in LSB; `%FS = mean/32768·100` | A large DC offset wastes headroom and can indicate a codec/ADC issue. |
| **Clipping** | Samples with `|sample| ≥ 32760` (≈0.999 FS); count and percentage | Also use device `CLIPPED` flag as a second opinion. |
| **Zero-crossing rate (ZCR)** | `crossings / (N−1)` per frame | For a sine, approximate frequency `≈ ZCR·sample_rate/2`; useful as a cheap pitch/noise indicator. |
| **VAD score** | 0–100 from the device; 255 = unknown | Energy VAD score is derived from the margin above the adaptive noise floor; device/chip VAD may be binary. |
| **Speech flag** | Device VAD decision, or server VAD decision | Used for noise-floor and SNR grouping. |

Important: frame stats are **aggregate across channels** in the device header.
The server computes per-channel values from interleaved PCM. For a stereo or
multi-mic capture, report per-channel and aggregate.

---

## 3. Segment-level metrics

A segment is bounded by `segment_start`/`segment_end`. The server computes:

| Metric | Definition | Notes |
|---|---|---|
| **Duration** | `samples_received / sample_rate` | Use audio-clock samples, not wall time. |
| **Frames expected** | From seq range/segment metadata | `max_seq - min_seq + 1` (with DISCONTINUITY-aware gaps). |
| **Frames received** | Count of binary frames | |
| **Frames lost** | Expected − received, excluding DTX/PLC and declared discontinuities | Report transport loss separately from device drops. |
| **Loss %** | lost / expected × 100 | |
| **Speech duration** | Samples flagged SPEECH / rate | For server VAD, use server decisions. |
| **Silence duration** | Samples not flagged SPEECH / rate | Includes pre-roll and tail. |
| **Pre-roll duration** | PRE_ROLL samples / rate | |
| **Speech RMS** | RMS over speech-flagged frames | Percentile/mean; use a robust statistic (e.g. 90th percentile of frame RMS) to avoid silence dragging it down. |
| **Noise floor** | 10th percentile of frame RMS over silence-flagged frames; if absent, pre-roll/tail silence frames; if still absent, 10th percentile of all frames | State the method in the report. |
| **SNR** | `speech_RMS_dB − noise_floor_dB` | If SNR is low but speech RMS is healthy, the room is noisy. If both are low, the mic/gain is the problem. |
| **Level distribution** | Percentiles (10/50/90/99) of frame RMS | Detects AGC pumping, intermittent dropouts, and level drift. |
| **Dropout count** | Runs of near-zero samples (e.g. ≥2 frames with RMS < −80 dBFS) inside speech | Distinguishes digital dropouts from a quiet room. |
| **Near-silence frames inside speech** | Frames flagged speech but with RMS below noise floor + 6 dB | Possible VAD false positives or packet gaps. |
| **DC offset** | Mean over speech frames | |

The server must record whether noise floor came from device VAD silence,
server VAD silence, pre-roll, tail, or a global percentile.

---

## 4. Spectral metrics

Use a Hann window with 50% overlap and an FFT size that covers at least the
frame/segment analysis window (e.g. 1024–4096 samples at 16 kHz). Normalize the
window for power.

| Metric | Definition | Use |
|---|---|---|
| **Spectrum** | `10·log10(|FFT|²)` dBFS/bin | Report a median/average spectrum per segment; plot 0–Nyquist. |
| **Spectrogram** | STFT magnitude over time | Shows dropouts, hum, clicks, and time-varying noise. |
| **Spectral centroid** | `Σ f·P(f) / Σ P(f)` | Brightness/noise indicator. |
| **Spectral flatness** | geometric mean / arithmetic mean of `P(f)` | High for white/background noise, low for tonal speech. |
| **Rolloff** | Frequency below which 85% or 95% of energy lies | Bandwidth/anti-alias check. |
| **Band energies** | Energy in speech bands (e.g. 0–300, 300–1000, 1–4 kHz, 4–8 kHz) | Shows missing high bands (codec/sample-rate issue) or excessive low-frequency noise. |
| **Hum** | Energy at 50/60 Hz and harmonics relative to total or to the 0–500 Hz band | Electrical/grounding or codec issue. |
| **High-frequency ratio** | Energy above 4 kHz / total | Low for a muffled mic; high for hiss. |

For Opus captures, spectral metrics describe decoded audio (codec + path), not
the raw ADC. Label them accordingly.

---

## 5. Test-tone metrics

A test tone gives a known stimulus and therefore absolute measurements.

### 5.1 Frequency and amplitude

- Estimate the fundamental with a parabolic-interpolated FFT peak, or a
  Goertzel filter when the expected frequency is known.
- Report measured frequency, expected frequency, and frequency error in ppm:
  `(f_measured − f_expected)/f_expected × 1e6`.
- Report fundamental amplitude in dBFS (RMS of the fitted sinusoid) and peak.

### 5.2 THD and THD+N

With fundamental power `P1` and harmonic powers `P2..PN` (N up to ~10, or up to
Nyquist/`f0`):

- `THD = sqrt(P2 + P3 + ... + PN) / sqrt(P1)`
- `THD_dB = 20·log10(THD)`
- `THD+N = sqrt(P_total − P1) / sqrt(P1)` where `P_total` is total signal power
  in the analysis band; `THD+N_dB = 20·log10(THD+N)`.
- Report the analysis bandwidth, window, harmonic count, and whether the
  fundamental was notched or bins excluded.
- `SFDR`: ratio of the fundamental to the largest non-fundamental spur.

### 5.3 Sweep

- For a log/linear sweep, use the device's sweep metadata and either:
  - deconvolve with the known sweep (preferred when the playback path is
    deterministic), or
  - split into short windows and estimate the instantaneous frequency/level.
- Report magnitude vs frequency and, if loopback/known reference is available,
  phase/delay.
- For a stepped-sine sweep (recommended in v1), each frequency is a separate
  step; use the 5.1/5.2 metrics per step. This is more robust and easier to
  validate than deconvolution.

### 5.4 Loopback delay

For electrical loopback, cross-correlate the captured signal with the known
tone/sweep and estimate delay before computing response. For acoustic loopback,
the delay includes speaker/room/mic; report it but do not correct it unless the
analysis requires alignment.

---

## 6. Frequency response and gain/rate sweeps

### 6.1 Frequency response

- Stimulus: stepped sine at 1/3-octave or selected frequencies, or a log sweep.
- Measure the fundamental amplitude at each step (or the transfer function via
  deconvolution).
- Normalize to a reference (e.g. 1 kHz) or to the known electrical stimulus.
- Report:
  - magnitude response (dB vs Hz),
  - −3 dB bandwidth,
  - ripple over the speech band,
  - low-frequency rolloff (mic/AC coupling),
  - high-frequency rolloff (anti-alias/codec).
- Acoustic measurements include the speaker/room/mic; an electrical loopback
  isolates the codec/ADC/DAC. State the path.

### 6.2 Gain sweep

For gains `g1..gN` with the same stimulus:

- Report RMS/peak vs gain; expected slope is ~1 dB/dB until compression.
- Find the **clipping point**: gain where clipping percentage or THD rises
  sharply.
- Report noise floor vs gain; a good preamp's noise floor rises slower than the
  signal until the ADC noise dominates.
- Report SNR vs gain and recommend the gain with the best SNR without clipping.
- Report DC offset vs gain (some codecs shift bias with PGA changes).

### 6.3 Sample-rate sweep

For each supported rate:

- Report THD+N, noise floor, −3 dB bandwidth, and alias/spur levels.
- Ensure the stimulus is below Nyquist; use an anti-alias-aware analysis band.
- A rate that shows excessive images/aliases or a much worse noise floor is a
  red flag.
- Opus captures at a given rate are not equivalent to raw PCM at that rate;
  label them.

---

## 7. Opus and codec caveats

- Opus is lossy. THD+N/frequency-response numbers from Opus include codec
  artifacts and are not chip-grade. Use PCM for chip claims.
- The device may pass through chip-owned Opus (GX8006), in which case per-frame
  RMS/peak/DC are unavailable (`STATS_VALID` clear). The server decodes and
  computes metrics, but must label the capture as chip-Opus.
- Parse the Opus TOC to report mode/bandwidth/frame duration. Do not assume the
  negotiated `frame_ms` equals the packet duration.
- Preserve raw packets. If a packet fails to decode, mark the segment partially
  analyzed and report the packet loss; do not silently interpolate.
- For host-encoded Opus, report encoder bitrate/complexity/VBR settings. For
  chip Opus, report that the encoder is chip-owned and may not honor requested
  settings.

---

## 8. Transport metrics

These separate transport problems from microphone problems.

| Metric | Definition | Notes |
|---|---|---|
| **Sequence gaps** | Missing `seq` values per segment | `seq` counts produced frames, including dropped ones; a gap is real loss unless DISCONTINUITY/DTX explains it. |
| **Packet loss %** | gaps / expected × 100 | Exclude pre-roll burst timing but include pre-roll frames in expected counts. |
| **Arrival jitter** | Standard deviation of `(arrival_i − arrival_0 − audio_time_i)` over live frames | Exclude pre-roll (sent as a burst) and DTX/PLC. |
| **Max inter-arrival gap** | Largest wall-clock gap between live frames | Compare to frame duration; spikes indicate Wi-Fi/scheduler stalls. |
| **Clock skew** | Slope of device audio timestamp vs server arrival time over a long capture | Report ppm; do not resample unless configured. |
| **Device drops** | `frames_dropped`/`tx_drops` from `segment_end`/telemetry | Distinguishes device TX overflow from network loss. |
| **Reconnects** | Count and outage durations | Each reconnect creates a new capture. |

Pre-roll frames are intentionally sent as a burst at segment start. Exclude
them from jitter; include them in loss/sequence checks.

---

## 9. Diagnostic decision tree

Use this as a starting guide; thresholds are tunable and must be stated in the
report.

### 9.1 "No voice detected"

1. Is the stream present at all? Check session telemetry and frame arrival.
2. Is the level near digital silence (< −80 dBFS) even in continuous mode?
   - Check mic enable/gain, codec power, I2S clock, channel selection, and the
     device's mic path. On GX8006, check chip mic/VAD/pickup configuration.
3. Is the level healthy but the VAD never asserts?
   - Check the VAD engine/threshold; try `continuous` mode and server VAD.
   - On energy VAD, inspect noise floor and `threshold_db`.
4. Is there a constant tone/hum instead of voice?
   - Check for a stuck test tone, codec clock, or electrical interference.

### 9.2 "Voice is too quiet / too loud"

- **Too quiet**: speech RMS below the expected range, low SNR, but no clipping.
  Increase gain or check mic bias/PGA. Compare gain sweep results.
- **Too loud/clipping**: peak at 0 dBFS, clipping percentage > 0, THD rises.
  Reduce gain or speaker volume; check AGC.
- **Intermittent level**: level percentiles/AGC pumping; check AGC settings and
  gain changes during capture.

### 9.3 "Noisy"

- High noise floor with normal speech RMS: acoustic/electrical noise, mic
  self-noise, or a bad ground. Check hum (50/60 Hz), band energies, and flatness.
- Low SNR but low absolute noise: mic gain too low; raise gain and re-measure.
- Noise rises with gain: preamp/ADC noise; choose the gain with best SNR.

### 9.4 "Dropouts / choppy audio"

- Sequence gaps with high jitter: Wi-Fi/transport. Check RSSI, TX queue, and
  server arrival gaps.
- Sequence contiguous but near-zero runs: capture/driver dropout or chip issue.
  Check device `capture_overruns` and HAL stats.
- Pre-roll missing/truncated: VAD onset/pre-roll configuration, not transport.

### 9.5 "Chip quality"

- Use PCM and a fixed gain/rate. Run test tones and a gain sweep.
- Compare THD+N and frequency response across gains/rates; identify the
  clipping point and the noise-floor knee.
- Check DC offset and hum; these are often board/codec issues.
- Repeat measurements; require stability across runs before concluding.

---

## 10. Report outputs

Per segment, `metrics.json` should contain at least:

```json
{
  "segment_id": 12,
  "capture_id": "01J8Z4...",
  "format": "pcm_s16le",
  "sample_rate": 16000,
  "channels": 1,
  "gain_db": 18,
  "path": "mic",
  "duration_ms": 2800,
  "speech_ms": 2100,
  "pre_roll_ms": 300,
  "frames_expected": 140,
  "frames_received": 140,
  "frames_lost": 0,
  "loss_pct": 0.0,
  "jitter_ms": 0.8,
  "rms_dbfs": -28.4,
  "peak_dbfs": -3.1,
  "crest_db": 25.3,
  "dc_offset_lsb": 3.2,
  "dc_offset_pct_fs": 0.01,
  "clip_pct": 0.0,
  "zcr": 0.08,
  "speech_rms_dbfs": -26.8,
  "noise_floor_dbfs": -61.2,
  "snr_db": 34.4,
  "noise_method": "device_vad_silence",
  "spectral_centroid_hz": 1850,
  "spectral_flatness": 0.12,
  "rolloff_85_hz": 3400,
  "hum_50_db": -72,
  "hum_60_db": -75,
  "dropouts": 0,
  "tone": null,
  "notes": []
}
```

Capture-level `summary.csv` should have one row per segment with the key fields
(segment id, label, reason, duration, loss, RMS, peak, clipping, noise, SNR,
THD+N/tone results, path, format, gain, rate). The HTML report embeds plots and
links to WAV/metrics/raw packets.

For test tones, add:

```json
{
  "tone": {
    "tone_id": "thd-1k",
    "waveform": "sine",
    "expected_hz": 1000,
    "measured_hz": 1000.4,
    "frequency_error_ppm": 400,
    "amplitude_dbfs": -6.1,
    "thd_db": -58.2,
    "thd_n_db": -55.7,
    "sfdr_db": 62.0,
    "analysis_band_hz": [20, 8000]
  }
}
```

All thresholds and analysis parameters (FFT size, window, harmonic count,
bandwidth, noise method) must be recorded so results are reproducible.

---

## 11. Test procedures

### 11.1 Microphone pickup (speech)

1. Quiet room; device at normal use distance.
2. Start a `continuous` capture at the default gain/rate.
3. Speak a known phrase at normal, soft, and loud levels; include 5 s of
   silence before/after.
4. Inspect level timeline, noise floor, SNR, clipping, and VAD decisions.
5. Repeat at 2–3 gains to find the best setting.
6. Confirm pre-roll captures the first phoneme in `vad` mode.

### 11.2 ADC/codec quality (PCM)

1. Use raw PCM, fixed sample rate, fixed gain.
2. Play a 1 kHz sine at −6 dBFS through the speaker or an external source.
3. Capture via mic (acoustic) or loopback (electrical).
4. Compute frequency, amplitude, THD, THD+N, SNR, hum, DC.
5. Repeat at several frequencies and gains; generate a frequency response and
   gain sweep.
6. Repeat the same measurement twice to check stability.

### 11.3 Speaker/DAC path

- Use an external microphone or electrical loopback to capture speaker output.
- Sweep tone frequencies and levels; check clipping, frequency response, and
  distortion.
- On GX8006, validate the speaker PCM/Opus path and flow control before trusting
  acoustic results.

### 11.4 Acoustic vs electrical loopback

- Acoustic: speaker → room → mic. Measures the whole product, not the chip
  alone. Use a quiet room and fixed geometry.
- Electrical: DAC/amp output → attenuator → ADC input (or vendor loopback).
  Isolates the codec/analog path. State the attenuation and path in the report.

---

## 12. Reference thresholds (tunable)

These are starting points, not product specifications. The server should make
them configurable and record them in the report.

| Check | Warning | Likely cause |
|---|---|---|
| Digital silence | RMS < −80 dBFS in continuous mode | Mic disabled, wrong channel, codec/I2S not running |
| Very low speech | Speech RMS < −50 dBFS | Gain too low, mic bias, distance |
| Clipping | clip_pct > 0.1% or peak ≥ 0 dBFS | Gain/volume too high, AGC |
| Noise floor | > −50 dBFS in a quiet room | Electrical/acoustic noise, bad ground |
| SNR | < 20 dB | Noise or low speech level |
| DC offset | > 1% FS | Codec bias/configuration |
| Hum | 50/60 Hz > −40 dB relative to speech band | Grounding/shielding |
| Dropouts | Any run ≥ 2 frames inside speech | Driver/chip/transport |
| Packet loss | > 0.5% | Wi-Fi/transport |
| Frequency error | > 1000 ppm (0.1%) | Clock/rate mismatch |
| THD+N (PCM, 1 kHz) | Worse than −40 dB | Codec/analog issue (compare to expected) |

Do not use a single threshold to declare a chip “bad”; compare against the
datasheet/board expectation and repeat measurements.

---

## 13. Formula summary

```
x[n]        = sample[n] / 32768
rms         = sqrt(mean(x^2))
rms_dbfs    = 20*log10(rms + 1e-12)
peak_dbfs   = 20*log10(max(|x|) + 1e-12)
crest_db    = peak_dbfs - rms_dbfs
dc_lsb      = mean(sample)
clip_pct    = 100 * count(|sample| >= 32760) / N
snr_db      = speech_rms_dbfs - noise_floor_dbfs
thd         = sqrt(sum(P_harmonics)) / sqrt(P_fundamental)
thd_n       = sqrt(P_total - P_fundamental) / sqrt(P_fundamental)
freq_error_ppm = (f_measured - f_expected) / f_expected * 1e6
jitter      = stddev(arrival_i - arrival_0 - audio_time_i)
clock_skew  = slope(audio_time vs arrival_time) - 1   (ppm)
```

Report the analysis window, FFT size, bandwidth, noise-floor method, and
pre-roll/tail handling alongside every metric so results are reproducible.
