# WiFi Audio Recorder + Diagnostic Streamer

A standalone WiFi audio recorder and analysis system for diagnosing microphone
pickup and audio-chip quality. **MQTT is always connected and controls when
recording starts and stops; there is no wake word.** While recording, the device
captures microphone audio, optionally segments speech with pre-roll, and streams
it over WebSocket to a Go analysis server that writes WAV files, computes
quality metrics, runs test-tone/sweep analysis, and produces reports.

The project is new and self-contained. [`embedded-iot-client`](lib/embedded-iot-client)
is included as a pinned git submodule for MQTT/IoT control. There is **no
ai-voice dependency**: the recorder implements its own audio, WebSocket, and
GX8006 code.

**Current status**

- **Linux device + Go server: implemented and verified end-to-end.** MQTT
  start/stop (TLS lab broker) opens the WebSocket data plane, PCM audio is
  captured, and the server writes valid WAVs, frame sidecars, metrics, plots,
  `summary.csv`, and `report.html`.
- **Protocol, segmenter, TX queue, GX8006 protocol/source, and app state
  machine: host-tested** (62 C++ tests) plus Go golden/metrics/tone/server
  tests.
- **LN882H + GX8006 platform: implemented and compiles** against the LN882H
  SDK (text ≈1.19 MB, bss ≈125 KB in the current build). Hardware bring-up
  (UART/DMA levels, handshake, continuous pickup, TLS heap, soak) is still
  pending; see [`docs/ln882h.md`](docs/ln882h.md).
- **No ai-voice dependency.** The recorder reimplements the small audio/WS/GX
  pieces it needs; the only upstream submodule is
  [`embedded-iot-client`](lib/embedded-iot-client) for MQTT control.

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
  embedded-iot-client's `AppInit`/`IoTCore`/thing-model path.
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
  decode, metrics, analysis, reports, HTTP control API, and an embedded
  React dashboard (`server/web`).

See [`docs/architecture.md`](docs/architecture.md) for the module and threading
details.

---

## Important design decisions

- **MQTT controls start/stop; WebSocket carries audio.** MQTT is always on so
  the operator can start recording while the data plane is disconnected. The
  control path reuses embedded-iot-client's `AppInit`/`IoTCore`/thing-model
  datapoints (`recorderCmd`/`recorderStatus`).
- **WebSocket is on demand by default** (`ws_mode=on_demand`): connect on MQTT
  `start`, disconnect after `stop` + idle timeout. This saves RAM/power,
  especially on LN882H.
- **MQTT owns start/stop by default**; WS `start_capture`/`stop_capture` are
  nacked unless explicitly allowed for debug.
- **No wake word/KWS.** Capture starts on MQTT; GX8006 KWS and ESP WakeNet are
  not enabled.
- **LN882H/GX8006 is Opus-only in the first milestone.** The chip protocol enum
  lists PCM, but the reference implementation/docs indicate microphone output
  is Opus in practice. PCM is enabled only after a bench test proves it. The
  server performs VAD, pre-roll, and segmentation for continuous Opus.
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

## Repository layout

```
ai/audio-probe/
├── README.md
├── CLAUDE.md
├── docs/
├── protocol/            # C++ header + golden vectors
├── src/
│   ├── core/            # app/client/segmenter/encoder/tone/telemetry/control
│   └── platforms/       # linux/, ln882h/
├── lib/embedded-iot-client/  # git submodule (MQTT/IoT core)
├── build/nix/           # Linux CMake
├── build/ln882h/        # LN882H SDK project + setup
├── tests/
├── scripts/             # vendor/setup/cert/build helpers
└── server/              # Go module: recorderd, analysis, reports
    └── web/             # React + Vite dashboard (embedded into the binary)
```

---

## Quick start

Dependencies are git submodules and pinned CMake FetchContent packages:

```bash
git submodule update --init --recursive   # embedded-iot-client + nested libs
```

Build and test the Linux device (dependencies are fetched on first configure):

```bash
cmake -S . -B out/nix -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/nix --target recorder recorder-tests -j
./out/linux/recorder-tests            # 62 host tests

cd server && go test ./...             # protocol/metrics/tone/WS-API tests

# Optional: decode Opus captures in the Go server (libopus via pkg-config).
# The default build preserves raw packets and marks them undecoded.
cd server && go test -tags "opus nolibopusfile" ./internal/audio
```

Run the full lab loop (MQTT TLS control + WebSocket data + Go server):

```bash
./scripts/gen_lab_certs.sh             # self-signed CA/server cert in out/certs
./out/linux/lab-broker -addr :1883 -tls-cert out/certs/server.crt \
  -tls-key out/certs/server.key
./out/linux/recorderd -addr :8080 -http :8081 -out ./server/data
./out/linux/recorder --source mic --server ws://127.0.0.1:8080/v1/recorder \
  --device-id recorder-linux --mqtt-ca out/certs/ca.crt

# Start/stop over the OneJSON/thing-model MQTT mapping
./out/linux/recorderctl -ca out/certs/ca.crt -action start \
  -params '{"mode":"continuous","label":"run-1"}'
./out/linux/recorderctl -ca out/certs/ca.crt -action stop
```

The stock embedded-iot-client Paho client always dials `ssl://`, so the lab
broker must be TLS-enabled for the device MQTT path. The WebSocket data plane
uses `ws://` for the lab; for WSS, add
`-cert out/certs/server.crt -key out/certs/server.key` to `recorderd`, run the
device with `--server wss://localhost:8080/v1/recorder --ca out/certs/ca.crt`,
and keep `-cert/-key` out of production scripts only if a trusted TLS terminator
is in front. The WSS + CA path is covered by the saved end-to-end run.

The dashboard is at `http://localhost:8081/`; captures/reports are written under
`server/data/captures/<capture_id>/`. The device's `recorderCmd` is a
thing-model internal datapoint and never routes to a serial MCU path.

## Frontend

The React + Vite app lives in [`server/web`](server/web). It lets you:

- browse captures and their segments;
- play the segment WAV in the browser;
- view level, transport, spectrum, tone, and server-VAD utterance metrics;
- inspect the waveform/spectrum plots and download raw sidecars;
- see live devices and their last telemetry;
- start/stop recording from the UI (subject to control ownership).

The production build is embedded into the Go server (`server/web/embed.go`), so
`recorderd` serves it at `/`. Rebuild it after UI changes:

```bash
./scripts/build_web.sh        # or: make web
cd server && go build ./...
```

`server/web/dist` is generated by the build and is embedded with `go:embed`
(`server/web/embed.go`). Keep it in the repository (it is small) so a clean
`go build` works; if it is absent, run `make web` first. For UI development, run
`cd server/web && npm install && npm run dev`; Vite proxies `/api` and `/data`
to the Go server on `:8081`.

**Recording controls.** The buttons call the server's
`POST /api/v1/devices/{id}/commands` with `start_capture`/`stop_capture`. By
design MQTT owns start/stop by default, so the server returns
`control_owned_by_mqtt` unless the device explicitly allows WS control. For
lab/frontend control, run the device with:

```bash
./out/linux/recorder ... --ws-control --ws-always
```

`--ws-control` sets `allow_ws_capture_control=true`; `--ws-always` keeps the
WebSocket session connected while idle, so a `start_capture` can reach the
device. Without `--ws-always`, an idle on-demand device has no WebSocket session
and the API reports that the device is not connected; start it over MQTT
instead. MQTT remains the authoritative control plane in production.

For LN882H, see [`docs/ln882h.md`](docs/ln882h.md): credentials are CMake
variables baked into a generated `platform_config.h`; the first milestone uses
Opus-only capture and server-side VAD. Until hardware bring-up is complete, the
capability matrix marks chip-PCM and test-tone as unproven.

```bash
export CROSS_TOOLCHAIN_ROOT=/usr       # or the official ARM toolchain root
RECORDER_WIFI_SSID=... RECORDER_WIFI_PASSWORD=... \
RECORDER_MQTT_ENDPOINT=... RECORDER_MQTT_DEVICE_ID=... \
RECORDER_MQTT_DEVICE_SECRET=... RECORDER_MQTT_CA_FILE=out/certs/ca.crt \
RECORDER_WS_URL=ws://192.168.1.100:8080/v1/recorder \
./scripts/build_ln882h.sh
```

---

## Roadmap

1. ✅ Protocol freeze + golden vectors.
2. ✅ Linux device core with synthetic/host tests.
3. ✅ Linux end-to-end + MQTT control (`recorderCmd`/`recorderStatus`) + Go
   transport/WAV/basic metrics.
4. ✅ Go analysis server (FFT/THD/tone/reports/dashboard); gain/rate sweep
   orchestration is partially implemented (tone tests work; sweeps are
   rejected while MQTT owns start/stop).
5. 🟡 LN882H platform implemented and compiles; hardware spike (UART DMA
   levels, PCM-vs-Opus, heap/stacks, TLS coexist, 10-minute soak) pending.
6. ⬜ LN882H diagnostics end-to-end.
7. ⬜ Test-tone/sweep acceptance on hardware.
8. ⬜ ESP32-S3, then BK7258/AC791/macOS/WT01Cx02.

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
