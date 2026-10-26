# Verification Plan

This plan covers the protocol, device core, Linux end-to-end path, Go analysis
server, and LN882H hardware. It is written to be executable once the
implementation exists; commands below are the target interfaces from
`docs/architecture.md`.

Status: design. No code exists yet.

---

## 1. Test layers

| Layer | Runs on | What it proves |
|---|---|---|
| Protocol golden vectors | Host (C++ and Go tests) | Both sides agree byte-for-byte on headers/JSON. |
| MQTT control tests | Host + broker | start/stop, status, dedup/expiry, ownership, fail-safe. |
| Device unit tests | Host, fake HAL/WS/MQTT | Segmenter/pre-roll/VAD/queue/timestamps/control. |
| Linux synthetic capture | Host | Signal processing and framing without hardware. |
| Linux end-to-end | Host + broker + mic/speaker or ALSA loopback | MQTT start/stop, WebSocket, WAV, metrics, reconnect. |
| Go analysis tests | Host | Metrics/THD/sweep/report correctness. |
| LN882H bring-up | Hardware | UART/DMA, GX8006, Wi-Fi, MQTT, WSS, heap/stacks. |
| LN882H diagnostics | Hardware | Continuous Opus, server VAD, soak, drops. |
| Test-tone/sweep | Linux then LN882H | Audio path quality measurement. |
| Security/regression | Host + hardware | Auth, CA, oversize payloads, backpressure. |

---

## 2. Protocol golden vectors

Create `protocol/golden/` with shared fixtures:

- `audio_header_pcm_s16le.bin`: a known 28-byte header + payload.
- `audio_header_opus.bin`.
- `audio_header_flags_all.bin`.
- `audio_header_sentinels.bin` (STATS_VALID clear).
- `hello_ln882h_opus.json`, `hello_linux_pcm.json`.
- `hello_ack_linux_pcm.json`, `hello_ack_ln882h_opus.json`.
- `segment_start_pcm.json`, `segment_end_pcm.json`.
- `mqtt_start.json`, `mqtt_stop.json`, `mqtt_status.json` (control envelope).
- `test_tone_command.json`, `sweep_steps.json`.
- Malformed cases: wrong version, payload length mismatch, oversized payload,
  unknown format, truncated header, expired/duplicate MQTT command.

C++ tests encode/decode each vector; Go tests do the same. The golden file is
the arbiter. Tests MUST fail if either side changes a byte.

Acceptance:

- [ ] Header size is exactly 28 bytes.
- [ ] All multi-byte fields are big-endian.
- [ ] Flags/format enum values match the spec.
- [ ] Sentinels and STATS_VALID behave as specified.
- [ ] Malformed vectors are rejected with the documented close/error code.
- [ ] JSON fixtures parse/serialize with no required field missing.
- [ ] MQTT command/status envelopes round-trip through the OneJSON string
      property and the self-hosted raw adapter.

---

## 3. MQTT control tests

Run against a test broker (Mosquitto for raw/self-hosted or a OneNET test
product) and a fake/real WS server.

### 3.1 Session and status

- [ ] Device MQTT connects; status is published on connect (current state).
- [ ] `recorderCmd` is registered as an internal datapoint and never reaches a
      serial MCU path.
- [ ] `start` transitions `idle → connecting_ws → handshaking → capturing`;
      status includes `capture_id`.
- [ ] `stop` finalizes the segment (`stop_capture`), transitions
      `capturing → draining → idle`, and closes WS after the idle timeout when
      `ws_mode=on_demand`.
- [ ] Status is published on every state change and command result.

### 3.2 Reliability

- [ ] Duplicate command `id` does not double-start; the current/last status is
      returned.
- [ ] `expires_at_ms` in the past returns `expired` and does not start.
- [ ] `start` while capturing returns the capturing status; `restart=true`
      stops/starts a new capture.
- [ ] `stop` while idle returns the idle status.
- [ ] MQTT disconnects while capturing: after
      `mqtt_lost_stop_timeout_ms`, capture stops and finalizes; status is
      published on reconnect.
- [ ] `max_capture_ms` stops a long capture and reports.

### 3.3 Ownership

- [ ] With `control.owner=mqtt`, WS `start_capture`/`stop_capture` return
      `nack`/`control_owned_by_mqtt`.
- [ ] With `allow_ws_capture_control=true`, both planes are serialized; MQTT
      `stop` wins over a concurrent WS start.
- [ ] WS `configure`/`test_tone`/`get_status`/`ping` still work under MQTT
      ownership.

### 3.4 Backend mappings

- [ ] OneJSON/thing-model: `recorderCmd`/`recorderStatus` string properties
      round-trip; a missing/wrong-type property produces a clear log/error and
      no crash.
- [ ] Self-hosted raw adapter: JSON envelope on the raw downlink is delivered
      to the recorder (not the serial path); status is published on the raw
      uplink.
- [ ] Broker restart/reconnect converges to the correct state; no unbounded
      recording.

## 4. Device unit tests (host, fake HAL/WS/MQTT)

Use fake `AudioSource`, fake transport, and a controlled clock. Do not sleep in
tests; advance time explicitly.

### 4.1 Segmenter and pre-roll

- Silence → speech → silence with known frame sizes.
- Verify:
  - `segment_start.t_ms` is the first pre-roll frame; `onset_t_ms` is the first
    speech frame.
  - PRE_ROLL frames appear exactly once, in order, before live frames.
  - `min_speech_ms` prevents a short blip from opening a segment.
  - `silence_ms` closes the segment and includes the trailing tail.
  - `max_segment_ms` forces a split.
  - `send_short_segments=false` discards short segments and counts them.
  - No duplicate frames at segment boundaries.
- Feed a `guard_ms` scenario and verify post-arm frames are ignored.

### 4.2 Sequence and timestamps

- Drop a produced frame in the TX path; verify `seq` still advances and the next
  sent frame has DISCONTINUITY.
- Verify `seq` resets per segment.
- Verify timestamps are sample-count-derived and monotonic within a segment.
- Verify pre-roll timestamps precede onset.
- Verify 32-bit wrap handling on the server side (see §7).

### 4.3 Queue/backpressure

- Fill the TX queue with control + audio records.
- Verify control records survive; audio drops oldest-first; counters increment.
- Verify capture never blocks and the segment ends with `queue_overflow` only
  under severe control loss.
- Verify packet boundaries are preserved (no merged/split Opus packets).

### 4.4 Format/rate changes

- PCM 16 kHz mono → 48 kHz stereo with capture idle.
- Opus 20 ms → 60 ms.
- Verify capture stops, segment ends `format_change`, new config is applied,
  and a new capture starts only on request.
- Verify an unsupported format returns `unsupported_format` and does not change
  state.

### 4.5 Command lifecycle

- Quick command ack/nack.
- `test_tone` accepted → completed/failed with the same id.
- Duplicate id does not re-execute.
- `ping` bypasses a blocked command queue.
- Unknown type gets a nack.
- Unsupported feature (set_gain on Linux) gets `unsupported_feature`.

### 4.6 MQTT control state machine

- Fake `IoTCore`/EventBus delivers `recorderCmd` start/stop and captures
  `recorderStatus` reports.
- Verify state transitions, status on connect/change, id echo.
- Duplicate id, expired command, start-while-capturing, stop-while-idle.
- MQTT-loss timeout stops capture; `max_capture_ms` stops.
- WS start/stop is rejected under MQTT ownership; allowed mode is serialized.
- No wake-word/KWS object is created or armed.

Acceptance:

- [ ] All tests deterministic (no real sleeps).
- [ ] ASAN/UBSAN clean.
- [ ] No allocation in the simulated capture path.
- [ ] Queue/segment invariants asserted.

---

## 5. Linux synthetic capture

Generate WAV/PCM fixtures and feed them through the capture/segmenter without a
microphone:

| Fixture | Expectation |
|---|---|
| Full-scale 1 kHz sine | RMS −3.01 dBFS, peak 0 dBFS, crest 3.01 dB |
| −6 dBFS sine | RMS −9.03 dBFS, peak −6 dBFS |
| −20 dBFS sine | RMS −23.03 dBFS |
| Sine + DC offset (+1000 LSB) | DC ≈ 1000 LSB, 3.05% FS |
| Clipped sine | clip_pct > 0, THD elevated |
| White noise at −40 dBFS | noise floor ≈ −40 dBFS, flatness high |
| Silence | RMS < −80 dBFS, no VAD |
| Silence → sine → silence | exactly one segment, correct pre-roll/tail |
| Short blip | discarded (default) or sent with send_short_segments |
| Two utterances with a gap | two segments, no overlap |
| Drop every 10th frame | loss/gaps reported, DISCONTINUITY flags |

Acceptance:

- [ ] Segment boundaries and counts match expectations.
- [ ] Level metrics within ±0.1 dB of the analytic value.
- [ ] Clip/DC/SNR within the documented tolerance.
- [ ] Header stats and server-recomputed stats agree within tolerance for PCM.
- [ ] Opus path decodes and labels lossy; chip-Opus stats are sentinels.

---

## 6. Linux end-to-end

### 6.1 Setup

Target commands (implementation pending). This example uses the self-hosted raw
MQTT mapping and a local Mosquitto broker; the OneJSON/thing-model path is
tested separately.

```
# Build
cmake -S build/linux -B out/linux
cmake --build out/linux
ctest --test-dir out/linux

# Control plane: local broker
mosquitto -p 1883

# Data plane: analysis server (no auto-start; MQTT owns control)
cd server
go run ./cmd/recorderd -addr :8080 -http :8081 -out ./data

# Device (MQTT + WS config; compile-time or config file)
./out/linux/recorder --config recorder.conf \
  --server ws://127.0.0.1:8080/v1/recorder
```

MQTT raw topics (self-hosted mapping):

```
DEV=bcbe50000006
PID=recorder-lab
# Start
mosquitto_pub -t "iot/$PID/$DEV/raw/down" -m \
  '{"v":1,"type":"recorder_cmd","id":"c1","action":"start","params":{"mode":"vad","label":"run-1"}}'
# Status (device publishes)
mosquitto_sub -t "iot/$PID/$DEV/raw/up" -v
# Stop
mosquitto_pub -t "iot/$PID/$DEV/raw/down" -m \
  '{"v":1,"type":"recorder_cmd","id":"c2","action":"stop"}'
```

For OneJSON platforms, send `recorderCmd` as a string property through the
platform's property-set API and subscribe to `recorderStatus`.

For deterministic audio, route the speaker to the mic with an ALSA loopback
device or use a physical loopback; otherwise speak into the mic.

### 6.2 Checks

- [ ] Device connects MQTT and publishes an initial `idle` status.
- [ ] MQTT `start` triggers `connecting_ws → handshaking → capturing`; the WS
      server sees `hello`/`hello_ack`/`capture_state`.
- [ ] MQTT `stop` finalizes the segment and returns `idle`; WS closes after the
      idle timeout when `ws_mode=on_demand`.
- [ ] Duplicate `id` does not double-start; expired command is rejected.
- [ ] WS `start_capture`/`stop_capture` is nacked under MQTT ownership.
- [ ] Speech produces a WAV + `meta.json` + `metrics.json`.
- [ ] Pre-roll captures the first phoneme; `onset_t_ms` is after
      `segment_start.t_ms`.
- [ ] Telemetry shows levels even when no segment is active.
- [ ] `continuous` mode streams and shows levels without VAD.
- [ ] `configure` to 48 kHz stereo works with idle capture and new capture.
- [ ] `set_gain` returns `unsupported_feature` on Linux and does not break the
      session.
- [ ] `test_tone` plays and captures; report contains tone metrics.
- [ ] Server-orchestrated gain/rate sweep produces labeled captures and a
      comparison report.
- [ ] Stopping capture finalizes WAVs; no zero-length/corrupt files.

### 6.3 Reconnect/backpressure/fail-safe

- [ ] Kill the WS server mid-segment: device ends `transport_lost`; if the MQTT
      session still expects recording, it retries; no duplicate audio.
- [ ] Restart the WS server: new session/capture; old partial WAV finalized.
- [ ] Disconnect MQTT while capturing: capture stops after
      `mqtt_lost_stop_timeout_ms` and finalizes; status appears on reconnect.
- [ ] Drop the Wi-Fi link (or block the port): capture never blocks; TX drops
      counted; reconnect backoff works.
- [ ] Send a payload >4096 bytes through the Linux transport: it must be
      rejected/queued correctly, not silently truncated.
- [ ] Slow disk/consumer: server read loop stays responsive; bounded queues.

### 6.4 Security

- [ ] WS token required: missing/wrong token closes 4401.
- [ ] WSS with a test CA works; wrong CA fails.
- [ ] MQTT TLS/CA works (or the documented develop-only plaintext exception is
      in effect).
- [ ] `ws://` only with the develop option.
- [ ] No credentials in the repo or logs.

---

## 7. Go analysis server tests

### 7.1 Unit tests

- Header codec golden vectors (encode/decode).
- JSON message validation and error codes.
- Sequence gaps/DISCONTINUITY/DTX handling.
- Timestamp wrap and clock-skew estimation.
- WAV writer round-trip for s16/s24/s32/float.
- Opus decode fixture (libopus) and raw-packet preservation on decode failure.
- Metrics:
  - full-scale sine RMS/peak/crest,
  - DC offset,
  - clipping,
  - noise floor/SNR,
  - spectral centroid/flatness/rolloff,
  - THD with synthesized harmonic,
  - THD+N,
  - frequency error,
  - gain/rate sweep comparison.
- Report generation: files exist, JSON/CSV are well-formed, HTML references
  existing plots.

### 7.2 Synthetic acceptance values

- Full-scale 1 kHz sine: RMS = −3.01 ± 0.05 dBFS; peak = 0 ± 0.05 dBFS.
- −6 dBFS sine: RMS = −9.03 ± 0.05 dBFS.
- 1 kHz + 2nd harmonic at −40 dB: THD ≈ −40 dB ± 1 dB.
- White noise: flatness near 1; noise floor matches RMS.
- Frequency error: 1000.5 Hz for a 1 kHz expectation ≈ +500 ppm.
- Gain sweep: synthesized level should increase ~1 dB/dB until clipping.

### 7.3 API tests

- `GET /api/v1/health` 200.
- `GET /api/v1/devices`, `/captures` list sessions.
- `POST /commands` returns 202 and the device ack is correlated.
- Under MQTT ownership, a `start_capture`/`stop_capture` command via the server
  API is rejected/forwarded as `control_owned_by_mqtt` (the server must not
  bypass MQTT control).
- `POST /tests` orchestrates a tone/sweep and creates labeled captures.
- Report/audio/metrics downloads return the right content types.
- Auth on the HTTP API (if enabled) rejects unauthorized requests.

---

## 8. LN882H hardware bring-up

### 8.1 Electrical/board

- [ ] UART1 TX/RX/FC pins verified against the board schematic.
- [ ] 1 Mbps, 8N1; DMA enabled; RX buffer ≥2048.
- [ ] GX8006 mic/speaker connected; speaker PA/volume known.
- [ ] Debug UART/logging available.

### 8.2 Handshake and format discovery

- [ ] VERSION_QUERY/response succeeds repeatedly.
- [ ] SET_MIC_CONFIG response received; requested format logged.
- [ ] Capture raw `GET_MIC_DATA` payloads and determine PCM vs Opus:
  - consistent PCM-sized lengths and intelligible PCM decode, or
  - variable small packets that decode as Opus.
- [ ] Record the result in `docs/ln882h.md` and the capability matrix.
- [ ] If Opus-only, confirm the server decodes the chip packets.
- [ ] If PCM, confirm sample rate/channels and max frame size; enable PCM
      capability only then.

### 8.3 Continuous pickup / chip VAD

- [ ] Configure `VAD=0`, `MIC=1`, pickup period, and `WAKE_NOTIFY` session
      re-arm; observe START/STOP. This is not a wake word/KWS.
- [ ] Confirm no GX8006 KWS/wake-word engine is armed; capture starts on MQTT.
- [ ] Run ≥10 minutes; document whether the pickup window expires and whether
      re-arm is needed.
- [ ] If continuous is unreliable, use chip VAD (`vad.engine: device`) and set
      `device_pre_roll_max_ms: 0`.

### 8.4 MQTT control

- [ ] MQTT connects with compile-time credentials and publishes initial status.
- [ ] `recorderCmd` start/stop works; `recorderStatus` reports state/capture.
- [ ] Duplicate/expired commands and MQTT-loss fail-safe behave.
- [ ] WS start/stop is nacked under MQTT ownership.
- [ ] Thing model or self-hosted raw mapping tested; broker restart converges.

### 8.5 Transport/security

- [ ] `ws://` bring-up on LAN (develop build only).
- [ ] `wss://` with CA verification; token accepted; wrong token rejected.
- [ ] MQTT TLS/CA behavior verified or documented as a develop-only exception.
- [ ] AP/broker/server restart: reconnect with backoff; new capture.
- [ ] MQTT-only idle and MQTT+WS recording TLS memory headroom measured; no
      reconnect failure loop.

### 8.6 Resources

- [ ] Free heap, minimum free heap, stack high-water marks logged for
      MQTT-only idle and MQTT+WS recording.
- [ ] No watchdog resets during a 10-minute soak.
- [ ] UART overruns/parse errors/drops counted and zero in a healthy run.
- [ ] Frame lengths >256 bytes preserved; odd Opus lengths preserved.
- [ ] Test tone path validated or `test_tone` omitted.

### 8.7 End-to-end

- [ ] MQTT start opens WS; server receives hello/telemetry and decodes WAVs.
- [ ] Speech capture is intelligible; levels/SNR reported.
- [ ] Continuous Opus + server VAD produces utterance files with server
      pre-roll.
- [ ] Gain changes and rate changes (if supported) are reflected in reports.
- [ ] Induced packet loss/drops match server counters.
- [ ] MQTT stop finalizes the segment and returns to idle.

---

## 9. Test-tone and sweep acceptance

- [ ] Device plays the requested frequency/amplitude/volume with fades.
- [ ] Server measures frequency within 1000 ppm (PCM; Opus tolerance wider).
- [ ] Amplitude matches the requested level within ±1 dB (path-dependent).
- [ ] THD/THD+N stable across repeated runs (document variance).
- [ ] Gain sweep: level increases monotonically until clipping; report
      identifies the clipping point and the best-SNR gain.
- [ ] Rate sweep: only advertised rates; THD+N/noise/bandwidth table generated.
- [ ] Frequency response plot uses the actual measurement path and labels
      acoustic vs electrical loopback.
- [ ] Report clearly labels Opus captures as lossy and not chip-grade.

---

## 10. Regression/security checklist

- [ ] No capture stall while the WS is down.
- [ ] No silent truncation at the platform's old send limit.
- [ ] Sequence gaps are distinguishable from device drops.
- [ ] Pre-roll is not duplicated or lost.
- [ ] Segment boundaries are never invented by the server.
- [ ] Reconnect creates a new capture; no duplicate audio.
- [ ] Format/rate changes only while idle.
- [ ] MQTT owns start/stop; WS start/stop is nacked unless explicitly enabled.
- [ ] Duplicate/expired MQTT commands and MQTT-loss fail-safe behave.
- [ ] No wake-word/KWS engine is initialized or armed.
- [ ] Auth/CA enforced for WS and MQTT; no credentials in repo.
- [ ] Malformed/oversized frames rejected with 1002.
- [ ] Unsupported features nack, never guessed.
- [ ] Vendor provenance verification passes.

---

## 11. CI and hardware matrix

### 11.1 CI (host)

- C++ format/lint, unit tests, ASAN/UBSAN.
- Go `go test ./...`, `go vet`.
- Protocol golden vectors (C++ and Go), including MQTT command/status.
- MQTT control tests with fake MQTT/IoT callbacks (no real broker required).
- Synthetic DSP fixtures.
- Provenance verification.
- Optional: Linux end-to-end with a virtual/loopback audio device and a local
  broker.

### 11.2 Hardware bench

- LN882H + GX8006 board, debug UART, Wi-Fi AP, server host.
- MQTT broker (Mosquitto or the chosen IoT cloud) and credentials.
- Speaker/mic path or electrical loopback with attenuator.
- External reference microphone/sound level meter for acoustic calibration
  (optional but recommended).
- Quiet room or acoustic enclosure for repeatable measurements.

### 11.3 Release gate

- [ ] All host tests pass.
- [ ] MQTT start/stop, ownership, dedup/expiry, and MQTT-loss fail-safe pass.
- [ ] Linux end-to-end passes.
- [ ] LN882H bring-up/diagnostics checklist passes.
- [ ] Capability matrix updated with measured values (including MQTT support).
- [ ] Reports reproducible from the committed fixtures.
- [ ] No credentials in the repository.
