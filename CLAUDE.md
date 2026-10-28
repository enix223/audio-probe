# CLAUDE.md — ai/recorder

Guidance for future agent sessions working in this repository.

## What this project is

A standalone WiFi audio recorder + diagnostic streamer:

- Device captures microphone audio (Linux first, LN882H + GX8006 first embedded
  target), optionally segments speech with pre-roll, and streams PCM or Opus
  over WebSocket.
- Go server writes WAV per segment, computes audio-quality metrics, runs
  test-tone/gain-rate sweep analysis, and produces reports.
- Reuses embedded-iot-client as a git submodule (`lib/embedded-iot-client`) for
  MQTT/IoT control. It does **not** depend on ai-voice; the small audio/WS/GX
  pieces the recorder needs are recorder-specific implementations.

## Current state

- Linux device + Go server implemented and verified end-to-end (MQTT TLS
  start/stop → WebSocket → WAV/metrics/plots/report).
- 62 C++ host tests and the Go test suite pass.
- LN882H + GX8006 platform implemented and compiles against the LN882H SDK;
  hardware bring-up/soak is still pending (see `docs/ln882h.md`).
- Dependencies are submodules + pinned FetchContent packages. There is no
  `third_party/upstream` snapshot tree and no ai-voice dependency.

Read these before making changes:

- `docs/plan.md` — implementation phases, decisions, risks.
- `docs/mqtt-control.md` — MQTT control plane (start/stop/status).
- `docs/protocol.md` — normative wire contract.
- `docs/architecture.md` — modules/threads/data flow.
- `docs/audio-quality-metrics.md` — metric definitions.
- `docs/ln882h.md` — GX8006/PCM-vs-Opus uncertainty and bring-up.
- `docs/porting.md` — platform contract/matrix.
- `docs/vendoring.md` — submodule pin, dependency policy.
- `docs/verification.md` — test plan.

## Non-negotiable invariants

1. **MQTT is the control plane; WebSocket is the data plane.** MQTT is always
   connected and owns start/stop by default. WS connects on demand for audio.
   WS `start_capture`/`stop_capture` must be nacked with
   `control_owned_by_mqtt` unless the device explicitly allows WS control.
2. **There is no wake word/KWS.** Capture starts on the MQTT `start` command.
   Do not enable GX8006 KWS or ESP WakeNet. GX8006 `WAKE_NOTIFY` is only a chip
   session re-arm.
3. **MQTT command/status uses `recorderCmd`/`recorderStatus` internal
   datapoints** via embedded-iot-client `AppInit`/`IoTCore`/thing model, like
   ai-voice. Never route `recorderCmd` to an MCU serial path. Enforce `id`
   deduplication, `expires_at_ms`, and the MQTT-loss stop timeout.
4. **JSON segment boundaries are authoritative.** Binary frames carry
   `segment_id` and `seq` but no START/END flags. Do not reintroduce boundary
   flags.
5. **`seq` counts every produced frame, including dropped frames.** A gap means
   real loss. Mark backpressure/capture gaps with `DISCONTINUITY`; do not count
   them as transport loss.
6. **Timestamps are audio-clock-derived** (sample counts / Opus TOC) relative
   to `capture_t0`, not wall clock. Never use LN882H's coarse clock for audio
   timing.
7. **The binary header is 28 bytes, big-endian, version 1.** Do not change field
   widths/order without a major protocol version. Flags/format values are in
   `docs/protocol.md`.
8. **One WebSocket binary message = one Opus packet or one PCM frame.** Never
   concatenate Opus packets or split a packet across messages.
9. **Frame-preserving TX queue.** Control records are never dropped while the
   link is alive; audio may be dropped oldest-first with counters and
   DISCONTINUITY. Capture/app tasks never call the transport send.
10. **LN882H/GX8006 is Opus-only in the first milestone.** PCM is enabled only
    after a bench test proves the chip outputs PCM. Do not advertise unverified
    formats.
11. **WSS and MQTT TLS with CA verification by default.** Plaintext is a
    develop-only option. Never commit Wi-Fi/MQTT/WS credentials, tokens, or CA
    private material.
12. **No upstream modifications.** Dependencies are git submodules
    (`lib/embedded-iot-client`, pinned `b2b145b`). Do not edit the submodule.
    External C/C++ libraries are pinned FetchContent packages. Changes to
    upstream behavior belong in recorder-specific code, not in the submodule.
13. **The server never invents segment metadata.** A missing boundary is a
    fault recorded in the report.

## Platform priorities

1. Linux (miniaudio + libwebsockets) — fast protocol/server iteration.
2. LN882H + GX8006 — first embedded target; Opus-only + server VAD initially.
3. ESP32-S3 (ES8311/INMP441), then BK7258/AC791/macOS/WT01Cx02.

Each port implements MQTT control (via embedded-iot-client `AppInit`/`IoTCore`/
thing model), `AudioSource`, transport writer, clock/config/system adapters,
and a truthful capability descriptor. No wake word/KWS. No platform-specific
branches in the protocol core.

## Build/run commands

```bash
# Dependencies
git submodule update --init --recursive

# Linux device + tests
cmake -S . -B out/nix -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/nix --target recorder recorder-tests -j
./out/linux/recorder-tests

# Go server/tools/tests
cd server && go test ./...
go build -o ../out/linux/recorderd ./cmd/recorderd
go build -o ../out/linux/lab-broker ./cmd/lab-broker
go build -o ../out/linux/recorderctl ./cmd/recorderctl

# React dashboard (server/web); dist is embedded into recorderd
./scripts/build_web.sh          # or: make web
cd server/web && npm run dev    # optional dev server, proxies /api + /data to :8081

# Lab control/data plane. Paho always uses ssl://, so the lab broker is TLS.
./scripts/gen_lab_certs.sh
./out/linux/lab-broker -addr :1883 -tls-cert out/certs/server.crt \
  -tls-key out/certs/server.key
./out/linux/recorderd -addr :8080 -http :8081 -out ./server/data
./out/linux/recorder --source mic --server ws://127.0.0.1:8080/v1/recorder \
  --device-id recorder-linux --mqtt-ca out/certs/ca.crt
./out/linux/recorderctl -ca out/certs/ca.crt -action start \
  -params '{"mode":"continuous","label":"run-1"}'
./out/linux/recorderctl -ca out/certs/ca.crt -action stop

# LN882H (hardware bring-up pending)
export CROSS_TOOLCHAIN_ROOT=/usr   # or the official ARM toolchain root
./build/ln882h/setup.sh            # or: make ln882h-recorder-setup
RECORDER_WIFI_SSID=... RECORDER_WIFI_PASSWORD=... \
RECORDER_MQTT_ENDPOINT=... RECORDER_MQTT_DEVICE_ID=... \
RECORDER_MQTT_DEVICE_SECRET=... RECORDER_MQTT_CA_FILE=out/certs/ca.crt \
RECORDER_WS_URL=ws://192.168.1.100:8080/v1/recorder \
./scripts/build_ln882h.sh          # or: make ln882h-recorder-build
```

The MQTT mapping is the OneNET OneJSON/thing-model path (`$sys/<product>/<device>/...`)
with `recorderCmd`/`recorderStatus` string properties. The stock self-hosted raw
core forwards downlink bytes to the MCU serial path and is not used.

## Dependency rules

- `lib/embedded-iot-client` is a git submodule pinned at `b2b145b`. The
  preferred `f787e55` tree lacks `RegisterInternalDatapoint`,
  `kEventInternalDatapoint`, `kEventDatapointReport`, and `NamedDatapoint`,
  which the MQTT control invariant requires. `b2b145b` is the commit the
  pinned ai-voice snapshot vendored, so its APIs match the recorder's expected
  composition. See `docs/vendoring.md`.
- Initialize nested submodules with
  `git submodule update --init --recursive` (rapidjson, cpp-httplib, nonstd).
- Do not modify the submodule. The recorder-specific MQTT control adapter lives
  in `src/core/control/`.
- The normal build requires embedded-iot-client's MQTT/IoT core, thing model,
  and command handler; BLE provisioning and OTA are disabled.
- External libraries are pinned FetchContent packages: miniaudio 0.11.21,
  opus 1.5.2, libwebsockets 4.3.3; embedded-iot-client fetches mbedTLS 3.6.0
  and Paho MQTT C 1.3.13.
- The recorder does not compile ai-voice code at all. The GX8006 and WebSocket
  transports are recorder-specific implementations.
- The `recorderCmd` internal datapoint must never be routed to the MCU serial
  path. The Linux/LN882H builds use the OneNET OneJSON/thing-model mapping.

## Protocol changes

- `docs/protocol.md` is normative. Update it before changing wire behavior.
- Keep C++ and Go golden vectors in sync (`protocol/golden/`).
- Unknown JSON fields are ignored; unknown message types nack. Unsupported
  features nack, never guess.
- A header/flag/required-semantics change needs a new protocol major version.

## Testing expectations

- Every change to framing/segmenter/queue needs a deterministic host unit test.
- Keep ASAN/UBSAN clean.
- MQTT control: start/stop/status, duplicate id, expiry, ownership, and
  MQTT-loss fail-safe; no wake word/KWS anywhere.
- Linux end-to-end: WAVs/metrics/reports for PCM and Opus; reconnect creates a
  new capture; >4096-byte payloads are not truncated.
- LN882H changes require the bring-up checklist in `docs/verification.md`,
  including UART DMA, Opus/PCM discovery, MQTT, WSS/CA, heap/stacks for
  MQTT-only and MQTT+WS, and a 10-minute soak.
- Update `docs/porting.md` capability matrices only with measured values.

## Documentation style

- State whether a capability is verified, unproven, or TBD. Do not present
  assumptions as facts.
- Distinguish PCM from Opus when discussing chip-quality claims.
- Record measurement path (mic/loopback), gain, rate, and loss in reports.
