# Implementation Plan

This is the project execution plan. The normative wire contract is
[`protocol.md`](protocol.md); architecture details are in
[`architecture.md`](architecture.md); tests are in
[`verification.md`](verification.md).

Status: documentation phase complete. No code has been written yet.

---

## 1. Scope

Build a standalone WiFi audio recorder and diagnostic server:

- Device captures microphone audio, optionally segments speech with pre-roll,
  and streams PCM or Opus over WebSocket.
- **MQTT is always connected and controls start/stop recording** (no wake
  word). WebSocket is the audio/data plane and connects on demand.
- Server records WAV per segment, computes mic/codec quality metrics, runs test
  tones and gain/rate sweeps, and produces reports.
- Reuse selected ai-voice and embedded-iot-client code by vendoring exact
  snapshots; do not modify upstream. MQTT control reuses embedded-iot-client's
  `AppInit`/`IoTCore`/thing-model path like ai-voice.
- First device targets: **Linux** (protocol/server validation) and **LN882H +
  GX8006** (first embedded target).
- Later device targets: ESP32-S3, macOS, BK7258, AC791, WT01C202/C302.

Locked decisions:

- **MQTT is the control plane and is always connected**; WebSocket is the data
  plane and connects on demand while recording. MQTT start/stop is the
  authority; WS start/stop is nacked unless explicitly enabled for debug.
- **No wake word.** Capture is started by MQTT, not KWS/WakeNet. GX8006 KWS is
  disabled; only its mic/VAD session control is used.
- Reuse embedded-iot-client the same way ai-voice does: `AppInit` + `IoTCore` +
  `ThingModelContext` + internal datapoints (`recorderCmd`/`recorderStatus`).
- Runtime-negotiated PCM/Opus, sample rate, channels, frame duration, and
  bitrate where supported.
- LN882H first milestone is **Opus-only** (GX8006 mic PCM is unproven) with
  server-side VAD/pre-roll; Linux uses PCM + energy VAD + device pre-roll.
- JSON segment boundaries are authoritative; audio-clock timestamps; seq counts
  produced frames including drops.
- Frame-preserving TX queue; capture never blocks on the socket.
- WSS/CA by default; `ws://` develop-only; bearer token. MQTT TLS/CA where the
  backend supports it.
- Full Go analysis server in this project.
- LN882H compile-time WiFi/MQTT first; BLE provisioning later.
- Gain/rate sweeps are server-orchestrated in v1.

---

## 2. Reuse summary

| Need | Source |
|---|---|
| HAL interfaces, audio frames/pipeline/filters, accumulator, SPSC ring | ai-voice `src/core/audio/` |
| Host Opus codec, Opus TOC | ai-voice `cl_opus_codec.*`, `cl_opus_toc.h` |
| VAD endpoint/energy logic | ai-voice `cl_vad_filter.h` |
| WS interface | ai-voice `cl_ws_client.h` |
| Linux miniaudio HAL + libwebsockets | ai-voice `src/platforms/linux/` |
| LN882H GX8006 protocol/speaker queue/WS | ai-voice `src/platforms/ln882h/` |
| Storage/config/network/system/threads/logging | embedded-iot-client `f787e55` (fallback `b2b145b`) |
| MQTT control: `AppInit`, `IoTCore`, `ThingModelContext`, internal datapoints | embedded-iot-client (same pattern as ai-voice `VoiceAppInit`) |
| Build patterns | ai-voice `build/nix`, `build/ln882h` |
| Test fakes | ai-voice `tests/` patterns |

Do not reuse ai-voice's cloud voice platform, `Application`, `AudioService`,
`SessionGateFilter`, i18n, wake-word/KWS, or voice-specific BLE provisioner.
The recorder has no wake word; capture is started over MQTT. See
[`architecture.md`](architecture.md) §6 and [`mqtt-control.md`](mqtt-control.md).

Vendoring policy and file manifest: [`vendoring.md`](vendoring.md).

---

## 3. Target layout

```
ai/recorder/
├── README.md
├── CLAUDE.md
├── docs/
├── protocol/            # C++ header, schemas, golden vectors
├── src/
│   ├── core/            # app/client/capture/segmenter/encoder/tone/telemetry/config
│   └── platform/        # linux/, ln882h/, later others
├── third_party/         # upstream copies, provenance.json, patches
├── build/               # Linux CMake, LN882H SDK project/setup
├── tests/
├── tools/
├── scripts/
└── server/              # Go module
```

---

## 4. Phases

### Phase 1 — Scaffold + protocol freeze

- Create repo layout, vendoring script, `provenance.json`,
  `scripts/verify_provenance.sh`.
- Write `docs/protocol.md` (done), `protocol/cl_recorder_protocol.h`, JSON
  schemas, golden binary/JSON vectors.
- Inspect embedded-iot-client `f787e55` against ai-voice `2f8818e`; pin or
  document fallback to `b2b145b`.
- Go protocol package + header codec + golden tests.
- Verify: provenance hashes pass; C++/Go parsers agree on every vector;
  malformed/oversized vectors rejected.

### Phase 2 — Linux device core with synthetic audio

- Implement `AudioSource` (Linux miniaudio), `RecorderSegmenter` (energy VAD,
  pre-roll, min-speech/silence), `RecorderEncoder` (PCM + host Opus),
  frame-preserving TX queue, header codec.
- Implement `RecorderApp`, telemetry, test tone, command state machine.
- Host unit tests with fake HAL/WS: pre-roll, min-speech discard, seq/timestamps,
  queue overflow/discontinuity, format/rate renegotiation.
- Verify: synthetic speech/silence produces expected segments; no duplicate
  pre-roll; ASAN/UBSAN clean.

### Phase 3 — Linux end-to-end + MQTT control + Go transport

- Implement `RecorderClient` (hello/ack, command queue, reconnect), Linux lws
  writer integration (no truncation, service-thread writes), file storage/POSIX
  runtime.
- Implement the MQTT control plane same as ai-voice's composition:
  `AppInit(factory)`, `IoTCore`, `ThingModelContext`, internal datapoints
  `recorderCmd`/`recorderStatus`, start/stop/status, MQTT-loss fail-safe,
  `ws_mode=on_demand`, and MQTT control ownership.
- Go WS receiver, capture/segment storage, WAV/sidecar writer, basic metrics,
  HTTP control API, dashboard.
- Verify: MQTT start opens WS and begins capture; MQTT stop finalizes and closes
  WS; duplicate/expired commands; WS start/stop nacked under MQTT ownership;
  16 kHz mono and 48 kHz stereo; PCM/Opus round-trip; >4096-byte payloads;
  server restart creates a new capture with no duplicate audio; ack ordering;
  continuous mode levels without VAD.

### Phase 4 — Full Go analysis server

- libopus decode; FFT metrics (spectrum/centroid/flatness/rolloff/hum); noise
  floor/SNR; THD/THD+N; frequency response; gain/rate comparison; reports/plots;
  test orchestration API.
- Fixtures: sine/noise/clipping/silence; fake device client.
- Verify: known 1 kHz tone at −6 dBFS yields expected frequency/RMS/THD+N/SNR;
  Opus fixture decodes; reports reproducible.

### Phase 5 — LN882H hardware spike

- UART1 DMA at 1 Mbps, GX8006 handshake, compile-time WiFi/MQTT, MQTT TLS +
  WSS/CA, heap/stack measurement. No wake word/KWS; mic/VAD session control
  only.
- Inspect actual `GET_MIC_DATA` bytes/lengths: prove or disprove PCM. If
  Opus-only, keep PCM out of capabilities and proceed with continuous Opus +
  server VAD. Measure max/odd-length frames and UART overrun.
- Verify: MQTT start/stop from a broker; handshake stable; mic frames reach the
  server; TLS reconnect survives AP/broker/server restarts; MQTT+WS memory
  headroom measured; no watchdog resets.

### Phase 6 — LN882H diagnostics end-to-end

- Continuous Opus upload, server-side VAD/pre-roll/segmentation, chip-VAD
  fallback, telemetry, queue overflow, gain changes, test tone.
- Verify: 10-minute soak with no unbounded heap loss; pickup window/START/STOP
  documented; decoded WAVs valid; gap counters match induced drops; frames
  >256 bytes preserved; test tone validated or capability omitted.
- If PCM is proven, add device energy VAD + pre-roll and advertise PCM in a
  follow-up.

### Phase 7 — Test reports and quality acceptance

- Linux then LN882H: tone through speaker + mic capture; server-orchestrated
  gain/rate sweeps.
- Verify: tone frequency/amplitude within tolerance; THD/SNR stable; gain sweep
  shows monotonic level and clipping point; rate sweep uses only supported
  rates; HTML/JSON/CSV reports link to raw audio.

### Phase 8 — Remaining platforms

- ESP32-S3 first (preallocated ES8311 read buffer), then BK7258/AC791/macOS/
  WT01Cx02, reusing their HAL/WS and reporting verified capabilities. No
  protocol changes.

---

## 5. Top risks and mitigations

| Risk | Mitigation |
|---|---|
| **GX8006 PCM contradiction (highest)** | Advertise Opus-only on LN882H until bench-proven; byte-oriented source; server VAD/pre-roll for Opus; PCM behind a verified capability flag. |
| Continuous GX8006 pickup window | Configure `VAD=0`/`MIC=1`/pickup period/wake-notify; soak 10 min; re-arm if needed; fall back to chip VAD with no controlled pre-roll. |
| GX8006 frame truncation/odd bytes/UART overrun | Recorder-specific byte queue, exact lengths, max frame ≥768 B, UART DMA, RX ≥2048, flow control, drop counters. |
| **MQTT + WS RAM/TLS pressure (LN882H)** | MQTT is always on and WS on demand; MQTT TLS + `ws://` lab bring-up; measured heap/stacks; bounded queues; retain reconnect headroom; don't shrink mbedTLS input blindly. |
| MQTT control reliability | Internal datapoints + thing model (`recorderCmd`/`recorderStatus`); QoS 1 where available; `id` dedup; `expires_at_ms`; MQTT-loss stop timeout; status on connect. |
| Control-plane conflict | MQTT owns start/stop by default; WS start/stop nacked `control_owned_by_mqtt`; explicit debug opt-in. |
| libwebsockets 4096-byte cap / thread misuse | Patch Linux transport; queue complete messages; write only from lws service context; test >4096 B. |
| Blocking WS send stalls capture | Frame-preserving TX queue + dedicated writer; capture never sends. |
| Opus hides chip artifacts | PCM preferred where available; label Opus lossy; server-recomputed metrics; no chip-grade THD from Opus. |
| Server Opus decode dependency | Reference libopus via cgo; `noopus` build tag preserves raw packets; pure-Go fallback only if validated. |
| Security (vendor WS/MQTT CA) | Add CA verification, bearer/tokens, WSS/MQTT TLS default, develop-only plaintext; no credentials in repo. |
| Protocol drift | Versioned header, capability negotiation, shared golden vectors, nack unsupported features. |
| Upstream drift | Pin commits, provenance hashes, minimal patches, verify script. |

---

## 6. Acceptance criteria

- Protocol golden vectors pass on host and against the server.
- MQTT start opens/uses the WS data plane and begins capture; MQTT stop
  finalizes the segment and returns to idle; duplicate/expired commands and
  MQTT-loss fail-safe behave.
- Linux PCM capture produces valid WAVs, metrics, and reports; reconnect and
  backpressure behave.
- Go server computes correct levels/THD/SNR/sweep results on synthetic fixtures.
- LN882H: Opus capture reaches the server; server VAD/pre-roll works; UART DMA,
  WSS/CA, heap/stacks, and a 10-minute soak pass; capabilities reflect measured
  facts.
- Test tone/sweep results are stable and reported with measurement path,
  format, gain, and rate.
- No credentials in the repository; provenance verification passes.

---

## 7. Immediate next step

Implement Phase 1: create the protocol header/schemas/golden vectors, the
vendoring/provenance scaffold, and the Go protocol package. Then build the Linux
device core (Phase 2), the MQTT control plane + server transport (Phase 3), and
only then touch LN882H hardware.

Do not start firmware or server code before the protocol vectors, MQTT control
contract, and vendoring provenance are in place.
