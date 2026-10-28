# Recorder Architecture

This document describes how the recorder is structured, how its threads and
queues are owned, and how the device and server fit together. The wire contract
is in [`protocol.md`](protocol.md); this file is about the implementation.

Status: implemented for the Linux device + Go server, and for the LN882H
platform (compiled, hardware pending). The actual layout mirrors ai-voice:
`src/core/<module>/`, `src/platforms/linux/`, `src/platforms/ln882h/`,
`build/nix`, `build/ln882h`, and `lib/embedded-iot-client` as a submodule.
The module maps below remain accurate; paths are under `src/platforms/` (plural).

---

## 1. System context

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

The device is intentionally a thin, deterministic streaming client. It runs
MQTT control through embedded-iot-client (the same `AppInit`/`IoTCore` pattern
as ai-voice), but no cloud voice protocol. It does not decode audio for
analysis and has **no wake-word model**. The server owns the heavy analysis,
file management, and test orchestration.

Two device paths matter:

- **Linux/PCM path**: local PCM + energy VAD + device pre-roll. This is the
  fastest way to validate the protocol and the server.
- **LN882H/GX8006 path**: the front end currently exposes Opus only. The device
  streams continuous Opus and the server performs VAD, pre-roll, and
  segmentation. If a bench test proves PCM output, the Linux-style local path is
  enabled for LN882H too.

---

## 2. Device architecture

### 2.1 Module map

```
Embedded-iot-client composition (AppInit; same pattern as ai-voice)
├── Network / MQTT / IoTCore / ThingModelContext / CommandHandler / NTP
├── Storage / ConfigStore / SystemService / EventBus / scheduler / threads
└── BLE provisioning (optional; completes on MQTT connect)

RecorderApp (app task, scheduler/Poll)
├── RecorderMqttControl
│     recorderCmd/recorderStatus datapoints, command/status JSON,
│     start/stop intents, MQTT-loss fail-safe
├── RecorderConfig
│     server URL, token, device id, default audio format/gain
├── AudioSource
│     byte-exact CaptureFrame + source events
│   ├── LinuxAudioSource
│   │     HalAudioInLinux / miniaudio → PcmFrameAccumulator → optional host Opus
│   └── Gx8006AudioSource
│         UART frames → frame-preserving queue → Opus/PCM packets (recorder fork)
├── RecorderSegmenter
│     pre-roll ring, onset/min-speech/silence, max duration, labels
│     device VAD (energy/chip) or pass-through for server VAD
├── RecorderEncoder
│     PCM framing; host Opus; chip Opus bypass
├── RecorderClient
│     session state, hello/ack, command correlation, TX queue, reconnect
│   └── TransportWriter
│         LN882H: dedicated FreeRTOS task
│         Linux: libwebsockets service/writable context
├── RecorderTone
│     sine/square/sweep generator with fades; optional simultaneous capture
├── RecorderTelemetry
│     periodic levels/link/system stats
└── RecorderCommandQueue
      WS callback → bounded command ring → app task
```

### 2.2 AudioSource

`AudioSource` is the recorder's capture abstraction. It differs from ai-voice's
`HalAudioIn` in one critical way: it returns **byte-exact** payloads, not
`int16_t` reinterpretations.

```cpp
struct CaptureFrame {
  const uint8_t* data;      // PCM bytes or Opus packet
  size_t bytes;
  size_t samples;           // per channel; valid for PCM, TOC-derived for Opus
  uint32_t timestamp_ms;    // audio-clock time of first sample
  AudioFormat format;       // PCM_S16LE or OPUS
  SourceFlags flags;        // discontinuity, overflow, chip session start/stop
};

class AudioSource {
 public:
  virtual AvpErr Start() = 0;
  virtual AvpErr Stop() = 0;
  virtual AvpErr Read(CaptureFrame& out, uint32_t timeout_ms) = 0;
  virtual AvpErr Configure(const AudioConfig& cfg) = 0;
  virtual SourceStats Stats() const = 0;
  virtual ~AudioSource() = default;
};
```

`Read()` must never truncate. If the HAL produces a larger chunk than the
caller's buffer, the source keeps it in a frame-preserving queue and returns it
in pieces; it must not drop the tail.

#### LinuxAudioSource

- Wraps the vendored ai-voice `HalAudioInLinux` (miniaudio; ALSA/PulseAudio on
  Linux, CoreAudio on macOS).
- Reads into a preallocated buffer; no per-read allocation.
- Uses `PcmFrameAccumulator` to turn variable HAL chunks into negotiated frames.
- For PCM, returns PCM bytes directly.
- For Opus, uses the vendored `OpusAudioCodec` (host libopus) on the app task.
- The miniaudio callback only writes into the HAL ring; no VAD, encoding,
  logging, or allocation runs in the callback.

#### Gx8006AudioSource (LN882H)

This is a **recorder-specific fork**, not the voice-coupled `HalAudioGx8006`:

- Owns the GX8006 protocol object, UART RX task, and a frame-preserving byte
  queue.
- Every mic packet keeps its exact length, including odd-length Opus packets.
- Queue slots are sized for the measured maximum (initial target ≥768 bytes;
  never assume 256).
- Chip `START/MID/STOP` events become source events, not implicit audio bytes.
- `Stats()` exposes UART overruns, queue drops, truncations (must be zero), and
  chip reboots.
- For the first milestone it returns Opus packets from the chip. If PCM is
  bench-proven later, it can return PCM frames from the same queue.
- Speaker/test-tone output uses a separate frame-preserving speaker queue; a
  single feeder task owns chip flow control and START/STOP markers.

### 2.3 RecorderSegmenter

The segmenter is recorder-specific. It does not reuse ai-voice `AudioService`
or `SessionGateFilter`, both of which are voice-turn-oriented.

Responsibilities:

- Maintain the PCM pre-roll ring for device-VAD mode.
- Run the VAD decision:
  - `energy`: frame RMS vs adaptive noise floor.
  - `device`: GX8006/ESP-SR START/STOP events.
  - `server`: no local segmentation; continuous frames pass through.
- Feed `VadEndpointTracker` for onset/min-speech/silence logic.
- Emit segment open/close events and pre-roll frames exactly once.
- Enforce `max_segment_ms`, short-segment policy, and `segment_ms` splitting in
  continuous mode.
- Tag frames with `segment_id`, `seq`, `PRE_ROLL`, `SPEECH`, `DISCONTINUITY`,
  and `CLIPPED`.
- Keep `seq` monotonic per segment even when frames are dropped later.

Pre-roll sizing: the ring must cover
`pre_roll_ms + onset_ms + min_speech_ms` plus a small margin. At 16 kHz mono
s16, 500 ms is 16 KB; this is acceptable on Linux and should be budgeted on
LN882H if PCM is enabled. In LN882H Opus/server-VAD mode there is no device
pre-roll; the server keeps it.

Device-VAD onset flow:

1. Every frame goes into the pre-roll ring and through the VAD.
2. `VadEndpointTracker` reaches `SpeechSeen()` after `min_speech_ms`.
3. The segmenter emits `segment_start`, then all buffered frames (PRE_ROLL),
   then live frames.
4. When the tracker signals end (silence hangover), it emits the final frames
   and `segment_end`, then resets for the next segment.
5. Frames shorter than `min_speech_ms` are discarded or sent according to
   `send_short_segments`.

Server-VAD flow (LN882H Opus):

1. The device starts capture and sends continuous frames.
2. It emits `segment_start` for the capture/file block and `segment_end` at
   `segment_ms` or stop; it does not attempt utterance segmentation.
3. The server decodes, VADs, buffers pre-roll, and writes utterance files
   inside the capture.

### 2.4 RecorderEncoder

- PCM: payload is the raw PCM frame; no codec.
- Opus host: `OpusAudioCodec::Encode()` on the app task; one packet per frame.
- Opus chip (GX8006): pass through the chip packet unchanged; do not re-encode.
- Header stats are computed from PCM before encoding when PCM is available.
  For chip Opus, `STATS_VALID` is clear.
- `ParseOpusToc` is used to derive packet duration and to validate chip packets.

### 2.5 RecorderClient

Owns the WebSocket session state and the wire protocol:

- `hello` construction from `RecorderConfig` and platform capabilities.
- `hello_ack` validation and application.
- Command correlation (`id`), quick vs long lifecycle, duplicate suppression.
- `capture_id`, `segment_id`, and audio-clock timestamp assignment.
- JSON message construction/parsing (rapidjson for parsing; a small writer for
  output).
- Reconnect/backoff (1 s → 30 s with jitter), hello timeout, session reset.
- TX queue and transport writer ownership.

The client never blocks the capture path. All sends go through the TX queue.

### 2.6 RecorderTone

- Phase-continuous sine/square generator; linear/log sweep.
- Fade-in/fade-out to avoid clicks.
- Writes in small chunks to the speaker sink from the app task (or a
  low-priority tone task if a sink blocks).
- On GX8006, validate speaker PCM chunk limits/flow control first; otherwise
  encode the tone to the chip's speaker Opus format or do not advertise
  `test_tone`.
- When `capture: true`, starts a `test_tone` segment and records the mic (or
  loopback) simultaneously.
- Tone metadata is echoed in `segment_start.tone` for server analysis.

### 2.7 RecorderTelemetry

- Per-frame stats in the binary header (RMS, peak, DC, clip, VAD score).
- Periodic JSON telemetry (levels, VAD state, TX queue, drops, reconnects,
  optional RSSI/heap, uptime).
- `stats`/`status` counters for diagnostics and the server dashboard.
- Reports are rate-limited and produced on the app task, never in the audio
  callback or transport writer.

### 2.8 RecorderConfig

- Server URL, token, device id, default audio/capture settings.
- On LN882H: compile-time Wi-Fi credentials from a gitignored generated header;
  stored values via embedded-iot-client `ConfigStore`.
- On Linux: file-backed storage; CLI flags override defaults until the server's
  `hello_ack` takes authority.
- Never stores credentials in the repository.

### 2.9 RecorderMqttControl

The MQTT control plane is a thin adapter over embedded-iot-client's IoT stack;
the full contract is in [`mqtt-control.md`](mqtt-control.md).

Responsibilities:

- At boot (after `AppInit`):
  - `IoTCore::RegisterInternalDatapoint("recorderCmd")`.
  - Subscribe to `kEventInternalDatapoint`; parse the `recorderCmd` string JSON
    into a `RecorderCommand{action, id, params, expires_at_ms}`.
  - Publish the parsed command into the recorder command queue with
    `source: mqtt`.
  - Publish current status on `kEventCloudStatusChange` (connected).
- On recorder state changes, publish `recorderStatus` via
  `kEventDatapointReport` (`NamedDatapoint{"recorderStatus", json}`).
- Implement `start`/`stop`/`configure`/`status` actions by feeding the same
  serialized app command queue used by the WS client.
- Enforce `expires_at_ms`, duplicate-id suppression, and control ownership.
- Enforce the MQTT-loss fail-safe: if the broker link drops while capturing,
  stop after `mqtt_lost_stop_timeout_ms` and finalize the segment.
- Do not route `recorderCmd` to any MCU serial path; it is internal.

The OneJSON/thing-model path works with OneNET/Azure/AWS/Aliyun. A self-hosted
raw MQTT path requires a small recorder-specific IoTCore/raw adapter because the
stock self-hosted core forwards raw downlink bytes to the MCU serial path; see
[`mqtt-control.md`](mqtt-control.md) §3.2.

No wake-word engine is created. The AudioSource is opened/started by the
recorder state machine when an MQTT `start` arrives; KWS is disabled on
GX8006/ESP32 builds unless a future option explicitly enables it.

---

## 3. Threading and ownership

| Context | Owns | Rules |
|---|---|---|
| **App/capture task** | Recorder state, segmenter, encoder, command execution, telemetry, TX queue producer | Single owner of capture/segment state. May block briefly on HAL read; never on socket send. |
| **Transport writer** | TX queue consumer, socket writes | LN882H: dedicated FreeRTOS task. Linux: libwebsockets service/writable context. Only context allowed to write. |
| **WS receive callback** | Parse/copy incoming text into command queue | Must not touch audio, segmenter, or TX queue. No blocking work. |
| **MQTT callback / IoTCore** | Parse OneJSON property set, publish `kEventInternalDatapoint`; publish upstream reports | Must not touch audio. `recorderCmd` is internal; never route it to a serial MCU. |
| **EventBus thread** | Deliver `kEventInternalDatapoint`/cloud status to `RecorderMqttControl`; deliver `kEventDatapointReport` to `IoTCore` | `RecorderMqttControl` parses and posts to the app command queue; no audio work. |
| **Command queue** | Bounded SPSC ring; WS/MQTT/EventBus → app task | If full, nack/drop with a counter; never block the producer. |
| **GX8006 RX task** | UART reads, protocol parsing, frame/event enqueue | No audio processing beyond framing; no logging in the hot path. |
| **GX8006 speaker feeder** | Speaker queue consumer, flow control, START/STOP | Sole chip speaker writer; preserves packet boundaries. |
| **miniaudio callback** | PCM into HAL ring | No VAD, encoding, logging, or allocation. |
| **Telemetry/reporting** | App task timers | Rate-limited; no real-time formatting. |

### 3.1 Why a frame-preserving TX queue

A bare byte ring loses packet boundaries when a producer writes a partial
record or a consumer reads across two records. Opus packets must reach the
server as exactly one packet per WebSocket message; the queue therefore stores
`{kind, length, bytes}` records. Control records are prioritized over audio and
are never dropped while the link is alive.

### 3.2 Shutdown order

1. Stop new capture.
2. Close the active segment (`stop_capture`/`transport_lost`).
3. Drain or drop the TX queue and stop the transport writer.
4. Close the WebSocket.
5. Stop GX8006 RX/speaker tasks.
6. Stop the audio source.
7. Stop the scheduler/event loop.

This order prevents a producer from writing into a freed queue and prevents a
segment from being left open in the server.

---

## 4. Data flows

### 4.1 Linux PCM VAD

```
miniaudio callback → HAL ring → AudioSource::Read
  → PcmFrameAccumulator (fixed 20 ms) → Segmenter
  → header stats + PCM payload → TX queue → lws writer → server
  → server VAD/pre-roll/analysis (if configured) or file writer
```

### 4.2 LN882H Opus continuous/server VAD

```
GX8006 mic (Opus) → UART RX task → Gx8006AudioSource queue
  → Segmenter (pass-through; chip events informational)
  → header (STATS_VALID=0) + Opus payload → TX queue → WS writer → server
  → server decodes → VAD/pre-roll/segmentation → WAV + metrics
```

If PCM is proven on the GX8006:

```
GX8006 mic (PCM) → Gx8006AudioSource queue → accumulator
  → energy VAD + device pre-roll → PCM frames → server
```

### 4.3 Test tone

```
server test_tone → app task → RecorderTone generator
  → speaker sink (PCM or chip Opus) + simultaneous mic capture
  → test_tone segment → server analysis (THD+N, response, level)
```

### 4.4 MQTT control path

```
MQTT broker
  → IoTCore parses recorderCmd property
  → kEventInternalDatapoint (NamedDatapoint)
  → RecorderMqttControl parses JSON, checks id/expiry/ownership
  → app command queue
  → RecorderApp:
       start: ensure WS connected → hello/hello_ack → start capture
       stop:  finalize segment → drain → idle
  → status via kEventDatapointReport
  → IoTCore encodes upstream property (OneJSON/raw)
  → MQTT broker
```

MQTT is the start/stop authority by default. WS commands are still used for
config/test/status, but `start_capture`/`stop_capture` are nacked unless the
device explicitly allows WS capture control.

### 4.5 WebSocket command path

```
server text → WS callback → command queue → app task
  → validate state/config → apply → ack/nack/accepted/completed
  → TX queue → transport writer
```

`ping` bypasses the command queue. `test_tone` is a long command with
`accepted`/`completed` or `failed`.

---

## 5. Recorder state machine

```
Boot
  → NetworkUp
  → MqttConnected (control plane up; WS may be down)
  → Idle
       ├── MQTT start ──► ConnectingWs ──► Handshaking ──► Capturing
       ├── MQTT stop / max_capture / mqtt_lost ──► Draining ──► Idle
       └── fatal audio/WS error ──► Error
  → Reconnecting (WS backoff; MQTT reconnect is owned by IoTCore)
```

Rules:

- Only the app task changes state; MQTT and WS callbacks post commands.
- MQTT `start`/`stop` are the control authority by default (`control.owner`).
  WS `start_capture`/`stop_capture` are nacked unless
  `allow_ws_capture_control` is true.
- On `start`, the device ensures the WS is connected and performs
  `hello`/`hello_ack`; the server assigns `capture_id`. With
  `ws_mode=on_demand`, the WS is closed after `stop` + `ws_idle_timeout_ms`.
- A format/rate/channel change requires `Idle`; the client ends the segment
  with `format_change`, applies config, and starts a new capture only on a new
  start request.
- A start while capturing is idempotent unless `restart=true`; a stop while
  idle returns the idle status.
- Any WS transport loss ends the segment with `transport_lost`, stops capture,
  clears stale audio, and either retries (if the MQTT control session expects
  recording) or returns to `Idle`.
- If MQTT is lost while capturing, stop after
  `mqtt_lost_stop_timeout_ms` and finalize; `max_capture_ms` is always enforced.
- A fatal mic/audio error enters `Error`, sends `error`/status, and does not
  claim to be capturing.

---

## 6. Reuse and vendoring map

### 6.1 ai-voice reuse (retired)

The recorder no longer depends on ai-voice. The table below documents the
original reference map; the implemented code is recorder-specific under
`src/core/audio/`, `src/core/ws/`, `src/platforms/linux/`, and
`src/platforms/ln882h/`.

| Component | Use |
|---|---|
| `cl_hal_audio.h` interfaces | `HalAudioIn`/`HalAudioOut` abstraction for Linux; GX8006 uses a recorder-specific source. |
| `cl_audio_frame.h`, `cl_audio_filter.h`, `cl_audio_pipeline.h` | May be used for optional local filters, but the segmenter is custom. |
| `cl_pcm_accumulator.h` | Fixed-frame accumulation. |
| `cl_audio_ring.h` | SPSC byte ring for TX/queue building blocks. |
| `cl_opus_codec.{h,cpp}` | Host Opus encode/decode (Linux; MCU host encoding only if compiled/measured). |
| `cl_opus_toc.h` | Opus packet duration/validation. |
| `cl_vad_filter.h` | `VadEndpointTracker` + energy VAD logic. |
| `cl_pcm_level_meter.h` | Reference for level computation; the recorder writes its own per-frame telemetry. |
| `cl_ws_client.h` | WebSocket abstraction. |
| Linux HAL/WS | miniaudio + libwebsockets implementation, with the writer/truncation patch. |
| LN882H GX8006 protocol/speaker queue/WS | Protocol and transport reference; `Gx8006AudioSource` is a recorder-specific fork. |
| Test fakes | `tests/test_audio.cpp`, `tests/test_voice_platform.cpp` patterns. |

### 6.2 Do not reuse

- `AiVoicePlatform` and the cloud voice protocols (BaiduRTC/JoyInside/AIUI).
- ai-voice `Application` and its wake-word/TTS/barge-in state machine.
- `AudioService` session gating and 500 ms cooldown.
- `SessionGateFilter`, i18n, and any wake-word/KWS engine. **The recorder has no
  wake word**; capture is started by MQTT and stopped by MQTT/fail-safe.
- ai-voice's voice-specific BLE provisioner; the built-in MQTT-completion
  provisioner is the natural fit when MQTT is required.
- `HalAudioGx8006` unchanged (int16 mic path, 256-byte slots, voice coupling).
- `FrameRecorderSink` (it only logs; it is not a recorder).

### 6.3 Reuse from embedded-iot-client

- `Storage`/`ConfigStore` (Wi-Fi/MQTT credentials, device id, recorder
  URL/token).
- `SystemService` (uptime, sleep, reboot, watchdog, heap where available).
- `EventBus`, `TaskScheduler`, `Thread`, `Mutex`, `Semaphore`, `Queue`.
- `Logger`.
- **MQTT/IoT stack**: `MQTTClient` (Paho on Linux, SDK MQTT on MCUs),
  `IoTCore` (OneNET/Azure/AWS/Aliyun/self-hosted), `ThingModelContext`,
  `CommandHandler`, `NTP`, `HttpClient` when the backend needs a thing-model
  fetch.
- MQTT control registration uses `IoTCore::RegisterInternalDatapoint`,
  `kEventInternalDatapoint`, and `kEventDatapointReport`.
- BLE provisioning (optional; completes on MQTT connect) and OTA (optional).
- LN882H: storage/flash bridge, network, system, threading, serial, startup.
- The recorder deliberately does **not** use MQTT to forward audio or to
  control an MCU serial target; `recorderCmd` is an internal datapoint.
- Do not compile MQTT/HTTP/OTA/BLE if a specific build genuinely omits them,
  but the normal recorder build requires the MQTT/IoT core.

### 6.4 Vendoring rules

See [`vendoring.md`](vendoring.md). In short: copy exact files under
`third_party/upstream/`, record provenance/hashes, apply minimal patches under
`third_party/patches/`, never modify the sibling repositories.

---

## 7. Platform abstraction and porting

Each platform provides:

1. **AudioSource** — byte-exact capture, format configuration, gain, source
   events, stats.
2. **Speaker/tone sink** — optional; needed for test tones.
3. **Transport** — WebSocket writer/receiver with the platform's threading
   rules; TLS/CA.
4. **Clock** — audio-clock anchor and monotonic time.
5. **Config/storage** — server URL/token/device id and Wi-Fi credentials.
6. **System info** — uptime, heap (optional), RSSI (optional), watchdog.
7. **Capability descriptor** — verified formats, VAD engines, gain, test tone,
   loopback, limits.

A platform that lacks a feature omits it from `hello.capabilities`; it must not
advertise untested formats. The protocol core and segmenter are shared; only
these adapters are platform-specific. See [`porting.md`](porting.md).

---

## 8. Build integration

### 8.1 Linux

- Root `CMakeLists.txt` + presets delegate to `build/linux`.
- Sources: recorder core/platform, the recorder's own Linux audio/WS code, and
  the embedded-iot-client IoT stack (storage, config, system, eventbus,
  scheduler, threading, logger, serial, MQTT/IoT core, thing model, command
  handler, NTP, HTTP as needed).
- Targets: `recorder-core`, `recorder-platform-linux`, `recorder`,
  `recorder-unit-tests`.
- Dependencies: miniaudio, libwebsockets, libopus, Paho MQTT, mbedTLS/OpenSSL
  (Paho/HTTP), pthread. Use pinned `FetchContent` by default; allow system
  libraries. Mirror ai-voice's `build/nix` `iotclient-platform` target.
- Patch libwebsockets wrapper: no 4096-byte truncation; queue complete
  messages; write only from the lws service/writable context.
- Tests via `ctest`; ASAN/UBSAN preset. Fake MQTT/IoT callbacks exercise
  start/stop without a broker.

### 8.2 LN882H

- `build/ln882h/setup.sh` copies `project/recorder` into
  `$LN882H_SDK/project/recorder`, substitutes absolute paths, copies
  `proj_config.h`/partition files, and sets `USER_PROJECT=recorder`.
- Explicit source lists (no globbing): SDK startup/Wi-Fi/lwIP/mbedTLS/partition
  manager/FreeRTOS; embedded-iot-client IoT stack (storage, config, system,
  eventbus, scheduler, threading, serial, MQTT/IoT core, thing model,
  command handler, NTP, HTTP as needed); GX8006 protocol/speaker/WS; recorder
  core and platform entry. This mirrors ai-voice's LN882H project, minus the
  voice-specific files.
- UART1 DMA at 1 Mbps, RX ≥2048, flow-control pin.
- Compile-time Wi-Fi/MQTT credentials in a gitignored generated header. TLS is
  used where the backend requires it; `wss://`/WSS is preferred in production.
- BLE provisioning and OTA are optional; the normal build requires MQTT but not
  BLE/OTA.
- Measure stack high-water marks and heap; budget:
  `ws_recv` 4096, `ws_tx` 3072, `gx8006_rx` 4096, `gx8006_spk` 3072,
  recorder app 6144, plus MQTT/TLS/HTTP (verify, do not assume).

### 8.3 Other platforms

ESP32-S3 first, then BK7258/AC791/macOS/WT01Cx02. Reuse their ai-voice HAL/WS;
add a recorder `AudioSource`/transport adapter and capability descriptor. No
protocol changes.

---

## 9. Memory and real-time budgets

### 9.1 LN882H

- Total RAM: 295 KB. MQTT is now always connected, so plan for MQTT TLS plus
  (when recording) WS/WSS TLS, roughly 35–40 KB each.
- Budget items: MQTT/TLS, HTTP/thing model (if the backend needs it), WS TLS,
  lwIP buffers, WS RX/TX, GX8006 RX/speaker queues, UART DMA buffer, TX queue,
  stack high-water, and (if PCM is proven) pre-roll ring.
- Use `ws_mode=on_demand` (default) so WS TLS is freed after stop.
- Bring up with MQTT TLS + `ws://` on a trusted lab LAN; measure WSS with CA
  before enabling it. Keep the mbedTLS input buffer at 16 KB until the Go
  server's TLS record behavior is tested.
- Add a heap low-water log and a reconnect-headroom check; do not shrink buffers
  blindly. If both TLS connections do not fit, document the chosen
  control/data-plane security trade-off.

### 9.2 Real-time rules

- Never log or allocate in the miniaudio callback, UART RX hot path, or
  transport writer hot loop.
- No blocking socket writes from the app/capture task.
- Keep the TX queue bounded; drop audio, never block capture.
- Rate-limit all diagnostics.

---

## 10. Security model

- **MQTT (control plane)** uses TLS with CA verification where the backend
  supports it; unique device credentials; the broker/product authorizes who can
  publish `recorderCmd`. On a trusted lab LAN a documented develop-only
  plaintext exception is allowed.
- **WebSocket (data plane)** uses `wss://` with CA verification by default;
  `ws://` only in an explicit develop build.
- Bearer token in the WS handshake; the server verifies it.
- Patch the LN882H WS client to verify certificates (upstream ai-voice currently
  disables verification). Apply the same rule to the MQTT client.
- Credentials/tokens live in gitignored generated headers or storage, never in
  the repo.
- MQTT command/status JSON must not contain credentials or audio. Use
  `expires_at_ms`/`id` deduplication to resist replay/duplicates.
- The server/broker rate-limits auth failures and logs device identity/close
  reason.

---

## 11. Go server architecture

### 11.1 Packages

```
server/
├── cmd/recorderd/main.go       # flags, listeners, wiring
├── cmd/recorderctl/main.go     # optional CLI for the HTTP API
├── internal/config/            # flags/env, defaults, validation
├── internal/wsapi/             # device upgrade/auth, read loop, writer
├── internal/protocol/          # JSON envelope, 28-byte header, validation
├── internal/hub/               # connected devices/sessions, command correlation
├── internal/session/           # captures, segments, state, reconnect rules
├── internal/audio/             # PCM, WAV, Opus decode, resampling
├── internal/metrics/           # frame/segment/spectral/Opus metrics
├── internal/analysis/          # tone, sweep, gain/rate comparison
├── internal/report/            # HTML/JSON/CSV/plots
├── internal/storage/           # capture filesystem layout
├── internal/httpapi/           # REST control API, downloads, dashboard
└── web/                        # React + Vite dashboard (embedded via go:embed)
```

Dependencies: `github.com/coder/websocket`, `gonum.org/v1/gonum/dsp/fourier`,
`gonum.org/v1/plot`, reference libopus via cgo (`gopkg.in/hraban/opus.v2`).
A `noopus` build tag preserves raw packets without decoding; a pure-Go fallback
is only for validated SILK-only voice and is not the default.

### 11.2 Session/capture/segment model

- The Go server is the **data plane**; it is not the MQTT control plane. With
  `control.owner: mqtt`, it must not start/stop capture; it may still send
  `configure`, `test_tone`, `get_status`, and `ping`. It can optionally
  subscribe to MQTT status for the dashboard, but control remains MQTT-owned.
- A WebSocket connection is a **session**. With `ws_mode=on_demand`, a new
  session appears for each MQTT-driven recording start.
- A session contains one or more **captures**; a reconnect always creates a new
  capture.
- A capture contains **segments** (utterances, continuous blocks, test tones).
- The server never invents segment metadata. It may provisionally open a
  segment if audio arrives first, but a missing `segment_start`/`segment_end` is
  a fault recorded in the report.
- The server tracks sequence gaps, audio-clock timestamps, arrival times, and
  clock skew. Pre-roll bursts are excluded from jitter metrics.

### 11.3 Storage

```
data/captures/<capture_id>/
├── manifest.json
├── summary.csv
├── report.html
└── segments/<segment_id>/{meta.json,audio.wav,packets.bin,frames.csv,metrics.json,plots/}
```

PCM is written directly; Opus is decoded with libopus to WAV; raw packets are
preserved. Each segment sidecar includes device header stats, server-computed
metrics, loss/jitter, config, and tone/sweep labels.

### 11.4 Analysis pipeline

1. Parse/validate frames; track seq/timestamps; write raw packets.
2. Decode/append PCM; finalize WAV on `segment_end`.
3. Compute segment metrics (levels, noise floor, SNR, spectral, clipping, DC,
   ZCR).
4. For test tones, compute frequency/amplitude/THD/THD+N/SFDR.
5. For server-orchestrated sweeps, compare steps and produce gain/rate tables.
6. Generate plots and the HTML report.

### 11.5 HTTP API and dashboard

- `GET /api/v1/health`
- `GET /api/v1/devices`
- `GET /api/v1/captures`
- `GET /api/v1/captures/{id}` and segment/report/audio/metrics downloads
- `POST /api/v1/devices/{id}/commands` (forward an allowed protocol command;
  202). Under MQTT control ownership, `start_capture`/`stop_capture` are
  rejected with `control_owned_by_mqtt`; the server must not bypass MQTT.
- `POST /api/v1/tests` (tone/gain sweep/rate sweep orchestration)
- `GET /` React dashboard (`server/web`, embedded): devices/captures, live
  telemetry, segment audio player, metrics/plots, and start/stop controls.
  Start/stop is MQTT-controlled by default and returns
  `control_owned_by_mqtt` unless the device reports `control.owner=ws` or
  explicitly allows WS control. Static files under `/data/`.

### 11.6 Server failure behavior

- A missing `segment_end` at disconnect finalizes the WAV from received frames
  and marks `transport_lost`.
- Decode failures preserve raw packets and mark the segment partially analyzed.
- Disk writes are buffered/queued; they must not stall the WebSocket read loop.
- Quotas bound message size, sessions, and disk use.

---

## 12. Failure handling summary

| Failure | Device behavior | Server behavior |
|---|---|---|
| MQTT drop while recording | Keep recording up to `mqtt_lost_stop_timeout_ms`, then stop/finalize; publish status on reconnect | Finalize segment; report control outage |
| MQTT start while WS down | Enter `connecting_ws`, attempt WS; status `error` `ws_unavailable` if it fails | No session until WS connects |
| WS starts while MQTT owns control | Reject/nack `control_owned_by_mqtt` | No state change |
| Duplicate/expired MQTT command | Return current status / `expired`; never double-start | Report id/status |
| Wi-Fi/WS drop | End segment `transport_lost`, stop capture, backoff, reconnect with new capture | Finalize partial segment, record outage |
| TX queue full | Drop oldest audio, count, DISCONTINUITY, report | Detect gaps; report loss |
| Mic/chip error | `error`, stop capture, `capture_state: error` | Mark capture failed; keep partial data |
| Unsupported config | `error`/`nack`, close 4400 | Choose from capabilities |
| Malformed frame | Close 1002 | Close 1002, log |
| Auth failure | Close 4401 | Reject handshake |
| Server restart | Reconnect/new capture | New session/capture |
| Opus decode failure | N/A | Preserve raw packet, mark partial |
| Short segment | Discard or send per config | Report discarded count if sent |

---

## 13. Design decisions and alternatives

- **MQTT control plane + WebSocket data plane**: MQTT is always connected so a
  remote operator can start/stop while idle; WS is connected on demand while
  recording to save RAM/power. This is the requested control model.
- **MQTT owns start/stop by default**: prevents the control and data planes from
  issuing conflicting starts/stops. WS start/stop is nacked unless explicitly
  allowed for debug/test.
- **No wake word**: capture is commanded over MQTT. This removes WakeNet/KWS
  models, their PSRAM/flash cost, and false-wake behavior; the audio source is
  gated by recorder state instead.
- **JSON control + binary audio** instead of all-JSON/base64: lower overhead,
  precise framing, easier sequence/timestamp handling.
- **Audio-clock timestamps** instead of wall clock: LN882H wall clock is coarse
  and may be unsynchronized; sample/TOC time is exact for analysis.
- **JSON boundaries as sole authority**: avoids split-brain when a binary
  boundary flag and a JSON boundary disagree.
- **Server-orchestrated sweeps in v1**: keeps firmware small; the server can
  change step definitions without a firmware update.
- **Server VAD for Opus-only front ends**: preserves pre-roll/segmentation
  diagnostics without adding an Opus decoder to the MCU.
- **Frame-preserving TX queue**: Opus packet boundaries are semantic; a byte
  stream would corrupt packets.
- **No mid-segment resume in v1**: at-most-once audio is simpler and avoids
  duplicate/dedup complexity; outages are reported.
- **Vendored sources, not submodules**: the project must build independently;
  provenance is tracked by manifest/hashes.
- **Full Go server now**: the protocol is validated end-to-end while the device
  port is still in progress; WAV/metrics/report behavior drives protocol
  requirements.
