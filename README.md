# WiFi Audio Recorder + Diagnostic Streamer

A standalone WiFi audio recorder and analysis system for diagnosing microphone
pickup and audio-chip quality. **MQTT is always connected and controls when
recording starts and stops; there is no wake word.** While recording, the device
captures microphone audio, optionally segments speech with pre-roll, and streams
it over WebSocket to a Go analysis server that writes WAV files, computes
quality metrics, runs test-tone/sweep analysis, and produces reports.

The project is new and self-contained. It reuses selected, proven code from
[`ai-voice`](../ai-voice) and
[`embedded-iot-client`](../../embedded-iot-client) by vendoring exact upstream
snapshots; it does not modify either upstream repository.

**Current status: documentation/design only.** No firmware, server, build, or
vendored source files exist yet. The next implementation step is the protocol,
MQTT control, and vendor scaffold described in
[`docs/protocol.md`](docs/protocol.md),
[`docs/mqtt-control.md`](docs/mqtt-control.md), and
[`docs/vendoring.md`](docs/vendoring.md).

---

## Why this exists

Two questions are hard to answer with a normal voice product:

1. **Is the microphone/module picking up voice correctly?** — level, noise
   floor, clipping, DC offset, dropouts, VAD behavior, and whether the first
   phoneme is lost.
2. **How good is the audio chip/analog path?** — gain linearity, noise vs gain,
   THD+N, frequency response, sample-rate behavior, and codec artifacts.

The recorder exists to answer both with raw data and repeatable measurements,
not just a listening test.

Key capabilities:

- **MQTT control plane**: remote start/stop/status, always connected, reusing
  embedded-iot-client's `AppInit`/`IoTCore`/thing-model path like ai-voice.
- WebSocket data plane over Wi-Fi, connected on demand, with JSON control and
  binary audio frames.
- **No wake word/KWS**: capture starts on the MQTT `start` command.
- Runtime-negotiated **PCM** or **Opus**, plus sample rate, channels, frame
  duration, and bitrate where the platform supports it.
- Speech-gated capture with **pre-roll**, or continuous capture for server-side
  VAD and mic-pickup diagnosis.
- Per-frame telemetry (sequence, audio-clock timestamp, RMS, peak, DC, clipping,
  VAD score) plus periodic levels/link/system stats.
- Local test tones and gain/rate sweeps for chip analysis.
- A full Go server: WAV per segment, metrics, test-tone/sweep analysis, plots,
  reports, and a small dashboard/control API.

---

## Architecture at a glance

```
                 MQTT broker / IoT cloud (control, always on)
                          ▲
                          │ start / stop / status
                          │
┌─────────────────────────┴────┐        WebSocket (data, on demand)        ┌──────────────────────┐
│ Device                       │ <=======================================> │ Go analysis server   │
│  MqttControl (AppInit/IoTCore)│                                          │  WS receiver         │
│  RecorderApp                 │                                          │  capture/segment     │
│  mic ──► AudioSource ──► VAD │                                          │  WAV + metrics       │
│          pre-roll ──► frame  │                                          │  reports/dashboard   │
│          encoder ──► TX queue│                                          │                      │
│  speaker ◄── test tone       │                                          │                      │
└──────────────────────────────┘                                          └──────────────────────┘
```

- **Device core** (C++17, platform-independent): audio source adapter,
  segmenter/pre-roll, encoder, frame-preserving TX queue, WebSocket session,
  MQTT control/status, command handling, telemetry, test-tone generator.
- **Platform adapters**: Linux (miniaudio + libwebsockets) and LN882H + GX8006
  first; ESP32-S3, BK7258, AC791, macOS, and WT01Cx02 follow.
- **Server** (Go): WebSocket receiver, capture/segment storage, PCM/Opus
  decode, metrics, analysis, reports, HTTP control API, dashboard.

See [`docs/architecture.md`](docs/architecture.md) for the module and threading
details.

---

## Important design decisions

- **MQTT controls start/stop; WebSocket carries audio.** MQTT is always on so
  the operator can start recording while the data plane is disconnected. The
  WS reuses embedded-iot-client's `AppInit`/`IoTCore`/thing-model datapoints
  (`recorderCmd`/`recorderStatus`) exactly where ai-voice uses them.
- **WebSocket is on demand by default** (`ws_mode=on_demand`): connect on MQTT
  `start`, disconnect after `stop` + idle timeout. This saves RAM/power,
  especially on LN882H.
- **MQTT owns start/stop by default**; WS `start_capture`/`stop_capture` are
  nacked unless explicitly allowed for debug.
- **No wake word/KWS.** Capture starts on MQTT; GX8006 KWS and ESP WakeNet are
  not enabled.
- **LN882H/GX8006 is Opus-only in the first milestone.** The chip protocol enum
  lists PCM, but ai-voice's implementation/docs indicate microphone output is
  Opus in practice. PCM is enabled only after a bench test proves it. The server
  performs VAD, pre-roll, and segmentation for continuous Opus.
- **Linux/PCM is the reference diagnostic path**: local PCM, energy VAD, device
  pre-roll, host Opus optional.
- **JSON `segment_start`/`segment_end` are the sole boundary authority.**
  Binary frames carry `segment_id`/`seq` but no start/end flags.
- **Timestamps are audio-clock-derived**, not wall clock: exact for analysis and
  independent of LN882H's coarse clock.
- **`seq` counts every produced frame, including dropped frames**, so a gap is
  real loss. Backpressure drops are marked `DISCONTINUITY`.
- **A frame-preserving TX queue and dedicated transport writer** keep capture
  from blocking on the socket; Opus packet boundaries are never merged or split.
- **WSS and MQTT TLS with CA verification by default**; plaintext is a
  develop-only option for a trusted lab LAN.
- **Gain/rate sweeps are server-orchestrated in v1**, keeping a sweep state
  machine out of the firmware.

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/plan.md`](docs/plan.md) | Implementation plan, phases, decisions, risks, and acceptance criteria. |
| [`docs/mqtt-control.md`](docs/mqtt-control.md) | MQTT control plane: start/stop/status envelope, thing-model/raw mappings, ownership, fail-safe. |
| [`docs/protocol.md`](docs/protocol.md) | Normative v1 wire protocol: transport, session, capabilities, binary header, segment semantics, JSON messages, errors, test modes, examples. |
| [`docs/architecture.md`](docs/architecture.md) | Device/server modules, threading, data flows, state machine, reuse map, build integration, memory budgets. |
| [`docs/audio-quality-metrics.md`](docs/audio-quality-metrics.md) | Metric definitions/formulas, test-tone/sweep analysis, diagnostic decision tree, report fields, thresholds. |
| [`docs/ln882h.md`](docs/ln882h.md) | LN882H + GX8006 UART/DMA, PCM-vs-Opus uncertainty, continuous pickup, memory/stacks, WSS, build, acceptance. |
| [`docs/porting.md`](docs/porting.md) | Platform contract, capability matrix, porting checklist, per-platform notes. |
| [`docs/vendoring.md`](docs/vendoring.md) | Upstream pins, file manifest, patches, provenance format, update policy. |
| [`docs/verification.md`](docs/verification.md) | Test layers, synthetic fixtures, Linux/LN882H procedures, acceptance criteria. |

---

## Target repository layout

```
ai/recorder/
├── README.md
├── CLAUDE.md
├── docs/
├── protocol/            # C++ header, JSON schemas, golden vectors
├── src/
│   ├── core/            # recorder app/client/capture/segmenter/encoder/tone/telemetry
│   └── platform/        # linux/, ln882h/, later ESP32-S3/BK7258/AC791/macOS
├── third_party/         # vendored upstream + provenance.json + patches
├── build/               # Linux CMake, LN882H SDK project/setup
├── tests/
├── tools/               # replay, golden-vector generator, WAV probe
├── scripts/             # vendor/verify scripts
└── server/              # Go module: recoderd, analysis, reports, dashboard
```

---

## Target quick start (not implemented yet)

These are the intended commands once the Linux device and Go server exist:

```bash
# Build the Linux device
cmake -S build/linux -B out/linux
cmake --build out/linux
ctest --test-dir out/linux

# Control plane: local MQTT broker
mosquitto -p 1883

# Data plane: analysis server (does not auto-start; MQTT owns control)
cd server
go run ./cmd/recorderd -addr :8080 -http :8081 -out ./data

# Run the recorder (MQTT + WS config; compile-time or config file)
./out/linux/recorder --config recorder.conf \
  --server ws://127.0.0.1:8080/v1/recorder

# Start recording over MQTT (self-hosted raw mapping)
mosquitto_pub -t "iot/PID/DEV/raw/down" -m \
  '{"v":1,"type":"recorder_cmd","id":"c1","action":"start","params":{"mode":"vad"}}'

# Stop recording
mosquitto_pub -t "iot/PID/DEV/raw/down" -m \
  '{"v":1,"type":"recorder_cmd","id":"c2","action":"stop"}'
```

The dashboard is at `http://localhost:8081/`; captures/reports are written under
`server/data/captures/<capture_id>/`. For OneJSON/thing-model platforms, send
`recorderCmd` as a string property through the platform API instead of the raw
topic.

For LN882H, compile-time Wi-Fi **and MQTT** credentials live in a gitignored
generated `platform_config.h`; see [`docs/ln882h.md`](docs/ln882h.md). The first
milestone uses Opus-only capture and server-side VAD.

---

## Roadmap

1. Protocol freeze + vendoring/provenance + golden vectors.
2. Linux device core with synthetic tests.
3. Linux end-to-end + MQTT control (`recorderCmd`/`recorderStatus`) + Go
   transport/WAV/basic metrics.
4. Full Go analysis server (FFT/THD/sweep/reports/dashboard).
5. LN882H hardware spike: prove PCM vs Opus, UART DMA, MQTT + WSS/CA,
   heap/stacks, no wake word.
6. LN882H diagnostics end-to-end (continuous Opus + server VAD).
7. Test-tone/sweep acceptance.
8. Remaining ai-voice platforms (ESP32-S3 first).

---

## Non-goals (v1)

- Wake-word/KWS detection. Capture starts on MQTT.
- Streaming audio over MQTT; MQTT is control/status only, audio goes over
  WebSocket.
- Mid-segment replay/resume after a network drop.
- Server-to-device audio playback (`play_audio` is reserved).
- Cloud voice-platform integration (Xiaozhi/BaiduRTC/JoyInside/AIUI).
- BLE provisioning in the first LN882H milestone (compile-time Wi-Fi/MQTT
  first).
- Encryption beyond TLS; multi-device synchronization.
