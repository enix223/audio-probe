# CLAUDE.md — ai/recorder

Guidance for future agent sessions working in this repository.

## What this project is

A standalone WiFi audio recorder + diagnostic streamer:

- Device captures microphone audio (Linux first, LN882H + GX8006 first embedded
  target), optionally segments speech with pre-roll, and streams PCM or Opus
  over WebSocket.
- Go server writes WAV per segment, computes audio-quality metrics, runs
  test-tone/gain-rate sweep analysis, and produces reports.
- Reuses selected code from `../ai-voice` and `../../embedded-iot-client` by
  vendoring exact snapshots. Never modify those upstream repositories.

## Current state

**Documentation/design only.** The docs describe the target implementation; no
firmware, server, build, or vendored source exists yet.

Read these before making changes:

- `docs/plan.md` — implementation phases, decisions, risks.
- `docs/mqtt-control.md` — MQTT control plane (start/stop/status).
- `docs/protocol.md` — normative wire contract.
- `docs/architecture.md` — modules/threads/data flow.
- `docs/audio-quality-metrics.md` — metric definitions.
- `docs/ln882h.md` — GX8006/PCM-vs-Opus uncertainty and bring-up.
- `docs/porting.md` — platform contract/matrix.
- `docs/vendoring.md` — upstream pins, file manifest, patches.
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
12. **No upstream modifications.** Vendor copies under `third_party/upstream/`
    with `provenance.json`, hashes, and patches. Keep patches minimal.
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

## Planned build/run commands

These are targets; they do not exist yet.

```bash
# Linux device
cmake -S build/linux -B out/linux
cmake --build out/linux
ctest --test-dir out/linux

# Control plane
mosquitto -p 1883

# Go data-plane server (MQTT owns start/stop)
cd server
go test ./...
go run ./cmd/recorderd -addr :8080 -http :8081 -out ./data

# Start/stop over MQTT (self-hosted raw example)
mosquitto_pub -t "iot/PID/DEV/raw/down" -m \
  '{"v":1,"type":"recorder_cmd","id":"c1","action":"start","params":{"mode":"vad"}}'
mosquitto_pub -t "iot/PID/DEV/raw/down" -m \
  '{"v":1,"type":"recorder_cmd","id":"c2","action":"stop"}'

# LN882H
make ln882h-recorder-setup
make ln882h-recorder-build
make ln882h-recorder-flash
make ln882h-recorder-monitor
```

## Vendoring rules

- Upstream pins: `ai-voice@2f8818e`; `embedded-iot-client@f787e55` preferred,
  `b2b145b` fallback only with documented API-drift reason.
- Copy exact files, preserve upstream-relative paths, record hashes in
  `third_party/provenance.json`, run `scripts/verify_provenance.sh`.
- Patches live in `third_party/patches/` with notes. Do not edit vendored files
  without a corresponding patch record.
- The normal build requires embedded-iot-client's MQTT/IoT core, thing model,
  and command handler; BLE provisioning and OTA are optional.
- Do not compile ai-voice `Application`, `AudioService`, cloud voice protocols,
  i18n, wake-word/KWS, voice-specific BLE provisioner, or `SessionGateFilter`.
- Do not compile `HalAudioGx8006` unchanged; implement the recorder-specific
  byte-oriented `Gx8006AudioSource`.
- The `recorderCmd` internal datapoint must never be routed to the MCU serial
  path. Self-hosted raw MQTT needs the recorder-specific adapter described in
  `docs/mqtt-control.md`.

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
