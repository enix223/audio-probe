# Recorder WebSocket Protocol — v1 Specification

Status: **design frozen for first implementation**. This document is normative for
the device and the Go analysis server. `docs/architecture.md` describes how the
implementation is organized; this file defines what goes on the wire.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are used as in RFC 2119.

MQTT control is specified separately in [`mqtt-control.md`](mqtt-control.md).
This document covers the WebSocket data plane and the shared JSON/binary
framing used by both planes.

## Contents

1. [Purpose and design goals](#1-purpose-and-design-goals)
2. [Terminology](#2-terminology)
3. [Transport](#3-transport)
4. [Session lifecycle](#4-session-lifecycle)
5. [Capability negotiation](#5-capability-negotiation)
6. [Audio packet format](#6-audio-packet-format)
7. [Segment semantics](#7-segment-semantics)
8. [JSON control messages](#8-json-control-messages)
9. [Telemetry and diagnostics](#9-telemetry-and-diagnostics)
10. [Timing and clock model](#10-timing-and-clock-model)
11. [Test tones and chip analysis](#11-test-tones-and-chip-analysis)
12. [Flow control and backpressure](#12-flow-control-and-backpressure)
13. [Security](#13-security)
14. [Versioning and compatibility](#14-versioning-and-compatibility)
15. [Informative: server file layout and report contract](#15-informative-server-file-layout-and-report-contract)
16. [Worked examples](#16-worked-examples)
17. [Appendix: capability matrix template](#17-appendix-capability-matrix-template)

---

## 1. Purpose and design goals

The recorder captures microphone audio, optionally segments it on speech, and
streams it to a server for two jobs:

1. **Microphone/module pickup diagnosis** — did the device hear the user, at
   what level, with what noise floor, clipping, dropouts, or DC offset?
2. **Audio-chip quality analysis** — gain linearity, noise vs gain, THD+N,
   frequency response, sample-rate behavior, and codec artifacts.

The protocol is designed around those jobs:

- **Negotiated audio format.** The server chooses PCM or Opus, sample rate,
  channels, frame duration, and (where the platform owns it) bitrate. The device
  advertises only formats it has actually verified.
- **Lossless PCM is the reference path.** PCM lets the server compute
  chip-grade metrics without codec artifacts. Opus is supported for bandwidth
  and for platforms whose front end only exposes encoded audio.
- **Speech-gated and continuous capture.** The device can upload only speech
  segments with pre-roll, or stream continuously for server-side VAD and
  mic-pickup diagnosis.
- **Per-frame telemetry.** Sequence, audio-clock timestamp, level, clipping,
  DC offset, and VAD score travel with each frame so transport loss can be
  separated from microphone problems.
- **Explicit capability negotiation.** Platforms differ substantially. Nothing
  is guessed and there is no silent fallback.
- **Diagnostic stimuli.** Local test tones and server-orchestrated gain/rate
  sweeps quantify the audio path.

Non-goals for v1: mid-segment resume after a network drop, server-to-device
audio playback (`play_audio` is reserved), multi-device synchronization, and
encrypted payloads beyond TLS.

---

## 2. Terminology

| Term | Meaning |
|---|---|
| **Session** | One WebSocket connection between a device and the server. |
| **Capture** | One logical recording run identified by `capture_id`. A capture may contain many segments. A reconnect always starts a new capture. |
| **Segment** | One audio unit group with a single `segment_id`, bounded by JSON `segment_start`/`segment_end`. A segment may be an utterance, a fixed continuous-capture block, or a test-tone block. |
| **Frame** | One PCM block or one Opus packet. Exactly one binary WebSocket message. |
| **Pre-roll** | Audio captured before the speech decision that opens a segment. |
| **Audio clock** | Device sample/TOC-derived time relative to `capture_t0`; not wall clock. |
| **VAD engine** | The component that decides speech: `energy` (device PCM), `device` (chip event), or `server` (server decodes and decides). |

---

## 3. Transport

### 3.1 Endpoint and framing

- WebSocket endpoint: `/v1/recorder` (servers MAY also accept `/ws`).
- **Text frames** carry UTF-8 JSON control messages.
- **Binary frames** carry exactly one recorder audio packet: a 28-byte header
  followed by the payload. One binary message MUST contain exactly one Opus
  packet or one PCM frame; Opus packets MUST NOT be concatenated.
- WebSocket message ordering is relied upon. A segment's JSON
  `segment_start` MUST be sent before its first binary frame, and
  `segment_end` MUST be sent after its last binary frame.
- The device and server MUST NOT send binary control messages in v1.

### 3.2 TLS and authentication

- Production/default: `wss://` with certificate-chain verification against a
  configured CA. `ws://` is allowed only in an explicit develop-only build.
- Authentication: `Authorization: Bearer <device-token>` in the WebSocket
  handshake. The server MAY require it; an invalid/missing token is rejected
  with HTTP 401 or WebSocket close code 4401.
- The device SHOULD also send `Device-Id: <id>` and
  `Protocol-Version: 1` handshake headers. They are informational; the
  authoritative identity/version is in `hello`.
- Tokens, Wi-Fi credentials, and CA private material MUST NOT be committed to
  the repository. Use a gitignored generated configuration header/template.

### 3.3 Message size limits

- `limits.max_frame_bytes` is advertised by the device in `hello` and by the
  server in `hello_ack`. The effective limit is the smaller of the two.
- v1 absolute limits: Opus payload ≤ 255 bytes; PCM payload ≤ 4096 bytes;
  `28 + payload_len <= max_frame_bytes`.
- A server MUST reject a frame whose `payload_len != len(message) - 28`, whose
  payload exceeds the limit, or whose size does not match the negotiated
  format/sample rate/channels/frame duration. Rejection closes with 1002; the
  device MUST NOT silently truncate.

### 3.4 Heartbeat and clock probing

- The device sends a JSON `ping` every 10 s while idle. The server replies with
  `pong` carrying the echoed `t0_ms`, its receive time `t1_ms`, and
  `server_time_ms`.
- The server MAY send `ping`; the device replies with `pong` using its own
  monotonic/audio clock values.
- `ping` bypasses the command queue and MUST NOT wait behind a long test.
- Platform-level WebSocket ping/pong is used in addition where available.

### 3.5 Reconnect

- There is **no mid-segment replay or resume in v1**.
- On transport loss, the device:
  1. Ends the active segment with `reason: transport_lost`.
  2. Stops capture and discards stale queued audio.
  3. Reconnects with exponential backoff (1 s → 30 s, with jitter).
  4. Performs a new `hello`; the server assigns a new `session_id` and
     `capture_id`.
- Audio is at-most-once. The server records the outage in the report. Buffered
  resume can be added in a future protocol version.

---

## 4. Session lifecycle

```
device                                      server
  |---- WebSocket connect (Bearer token) ---->|
  |---- hello -------------------------------->|
  |<--- hello_ack (session_id, capture_id) ----|
  |---- capture_state ------------------------>|
  |<--- start_capture -------------------------|   (unless hello_ack.start=true)
  |---- capture_state (capturing) ------------>|
  |---- segment_start ------------------------>|
  |---- AUDIO (binary, pre-roll) ------------->|
  |---- AUDIO (binary, speech/tail) ---------->|
  |---- segment_end -------------------------->|
  |<--- configure / set_gain / set_rate / tone-|
  |---- ack / nack --------------------------->|
  |<--- ping ---------------------------------|
  |---- pong --------------------------------->|
```

With `control.owner: "mqtt"` (the default), the start/stop commands arrive over
MQTT and this diagram begins at the data-plane handshake that follows an MQTT
`start`. The WS server does not auto-start; see §4.2 and
[`mqtt-control.md`](mqtt-control.md).

1. The device connects and MUST send `hello` within 5 s; otherwise the server
   closes with 4408.
2. The server validates version and capabilities and replies `hello_ack`. If it
   cannot choose a supported configuration, it closes with 4400.
3. The device applies the configuration. If `hello_ack.start` is true, it starts
   capture immediately; otherwise it sends `capture_state: idle` and waits for
   `start_capture`.
4. Capture proceeds through segments and commands until `stop_capture` or
   disconnect.

### 4.1 Command lifecycle

- Every server command MAY carry a numeric `id`. The device echoes it.
- **Quick commands** (`configure`, `start_capture`, `stop_capture`, `set_gain`,
  `set_rate`, `set_telemetry`, `get_stats`, `get_status`, `reset_stats`)
  produce exactly one terminal `ack` or `nack`.
- **Long commands** (`test_tone`; future `play_audio`) produce `accepted`, then
  exactly one `completed` or `failed`, all with the same `id`.
- Commands execute serially on the recorder app task. A duplicate `id` gets the
  same terminal response again without re-execution.
- `ping` bypasses the command queue.

### 4.2 Control ownership (MQTT vs WebSocket)

The recorder has two control paths:

- **MQTT** is the primary control plane when enabled. It carries start/stop and
  status; see [`mqtt-control.md`](mqtt-control.md).
- **WebSocket** is the data plane. It still carries `configure`, `test_tone`,
  `get_status`, `ping`, and the audio stream.

`hello.control` and `hello_ack.control` declare ownership:

```json
{
  "owner": "mqtt",
  "allow_ws_capture_control": false,
  "ws_mode": "on_demand",
  "mqtt_lost_stop_timeout_ms": 30000,
  "max_capture_ms": 1800000
}
```

- `owner: "mqtt"` (default when MQTT is enabled): the WS server MUST NOT start
  or stop capture. `start_capture`/`stop_capture` return `nack` with
  `code: "control_owned_by_mqtt"` unless `allow_ws_capture_control` is true.
- `owner: "ws"`: the WS server controls capture as in earlier sections; MQTT
  may still be connected for status only.
- `ws_mode: "on_demand"`: the device connects the WS when MQTT `start` arrives
  and may close it after stop + `ws_idle_timeout_ms`.
- `ws_mode: "always"`: the WS stays connected; it idles between recordings.
- `hello_ack.start` is honored only when `owner` is `ws`. With MQTT ownership,
  the device ignores it and starts only on the MQTT `start` command.
- Both planes report the actual state (`capture_state` on WS, status on MQTT);
  they never claim different states.
- If both are allowed (`allow_ws_capture_control: true`), commands are
  serialized on the app task and MQTT `stop` wins over a concurrent WS start.
- With `ws_mode: "on_demand"`, the WS server may see a new session for every
  recording start. It must not assume the WS was connected while the device was
  idle; `capture_id` is assigned in the `hello_ack` that follows the MQTT
  `start`.

---

## 5. Capability negotiation

### 5.1 Device `hello`

```json
{
  "type": "hello",
  "protocol_version": 1,
  "device": {
    "id": "bcbe50000006",
    "model": "ln882h-gx8006",
    "firmware": "0.1.0",
    "chip": "ln882h",
    "audio_frontend": "gx8006",
    "boot_id": "8f3c1a2e"
  },
  "clock": {
    "monotonic_ms": 123456,
    "wall_ms": 1760000000000,
    "synced": false,
    "capture_t0_mono_us": 0
  },
  "control": {
    "owner": "mqtt",
    "allow_ws_capture_control": false,
    "ws_mode": "on_demand",
    "mqtt_lost_stop_timeout_ms": 30000,
    "max_capture_ms": 1800000
  },
  "capabilities": {
    "formats": {
      "opus": {
        "sample_rates": [16000],
        "channels": [1],
        "frame_ms": [20, 40, 60],
        "bitrate": [6000, 64000],
        "payload_max": 255,
        "encoders": ["gx8006"]
      }
    },
    "vad": ["server", "device"],
    "gain": { "min": 0, "max": 42, "step": 6, "unit": "dB" },
    "loopback": false,
    "device_pre_roll_max_ms": 0,
    "server_pre_roll": true,
    "max_segment_ms": 30000,
    "tx_queue_ms": 3000,
    "limits": { "max_frame_bytes": 2048 }
  }
}
```

The example is the LN882H/GX8006 capability set: **Opus-only** until a bench
test proves PCM. `test_tone`/`speaker` are deliberately omitted until the GX8006
speaker path is validated. The device MUST advertise only formats and features
that have been verified on that hardware and build. A Linux build, for example,
advertises `pcm_s16le` (with rates/channels/frame durations/channel map) plus
`opus` with `encoders: ["host"]`.

`control` is a top-level object (not a capability): `owner` is `mqtt` or `ws`;
`allow_ws_capture_control` permits the WS server to start/stop when MQTT owns
control; `ws_mode` is `on_demand` or `always`; `mqtt_lost_stop_timeout_ms` and
`max_capture_ms` are fail-safe limits. See §4.2.

`capabilities` fields:

| Field | Required | Meaning |
|---|---|---|
| `formats` | yes | Map of `pcm_s16le` and/or `opus` to a format capability object. |
| `vad` | yes | Supported engines: `server`, `energy`, `device` (subset). |
| `gain` | optional | Native gain range/step/unit; omit if unsupported. |
| `test_tone` | optional | Supported waveforms/frequency range/max level; omit if no speaker/tone path. |
| `speaker` | optional | Volume range for test tones; omit if unsupported. |
| `loopback` | yes | Whether an electrical/on-chip capture loopback exists. |
| `device_pre_roll_max_ms` | yes | Maximum device-side pre-roll; `0` if the front end is gated/encoded. |
| `server_pre_roll` | yes | Whether continuous upload can be used for server-side pre-roll. |
| `max_segment_ms` | yes | Longest segment the device will produce. |
| `tx_queue_ms` | yes | Approximate audio buffering in the device TX queue. |
| `limits.max_frame_bytes` | yes | Largest binary message the device can send/receive. |

Format capability object:

| Field | Applies to | Meaning |
|---|---|---|
| `sample_rates` | both | Verified rates in Hz. |
| `channels` | both | Verified channel counts. |
| `frame_ms` | both | Supported frame durations. |
| `bitrate` | Opus | Informational range; chip-owned encoders may not honor server requests. |
| `payload_max` | both | Largest payload for this format. |
| `encoders` | Opus | `gx8006` (chip) and/or `host` (device CPU). |
| `channel_map` | PCM | Optional labels such as `["mic0","mic1"]`. |

### 5.2 Server `hello_ack`

```json
{
  "type": "hello_ack",
  "protocol_version": 1,
  "session_id": "01J8Z3...",
  "capture_id": "01J8Z4...",
  "server_time_ms": 1760000000123,
  "control": {
    "owner": "mqtt",
    "allow_ws_capture_control": false,
    "ws_mode": "on_demand"
  },
  "audio": {
    "format": "pcm_s16le",
    "sample_rate": 16000,
    "channels": 1,
    "frame_ms": 20,
    "bits_per_sample": 16,
    "endianness": "little",
    "interleaved": true
  },
  "capture": {
    "mode": "vad",
    "pre_roll_ms": 300,
    "max_segment_ms": 15000,
    "vad": {
      "engine": "energy",
      "onset_ms": 120,
      "silence_ms": 700,
      "min_speech_ms": 150,
      "guard_ms": 0,
      "threshold_db": 6,
      "send_short_segments": false
    }
  },
  "telemetry": {
    "frame_stats": true,
    "level_interval_ms": 500,
    "stats_interval_ms": 5000
  },
  "limits": { "max_frame_bytes": 2048, "max_queue_ms": 3000 },
  "start": true
}
```

The example is a Linux/PCM response; `gain_db` is omitted because Linux has no
hardware mic gain. For the LN882H example above, the server instead selects
`"format": "opus"` with `"opus": {"encoder": "gx8006"}`,
`"capture": {"vad": {"engine": "server", ...}}`, and a new `capture_id`; the
device streams continuous Opus and the server performs VAD/pre-roll/segmentation.

Rules:

1. The server MUST select exactly one advertised format/rate/channel/frame
   combination. There is no silent fallback.
2. If the requested configuration is unsupported, the device sends `error`
   with `code: "unsupported_format"` (or `unsupported_config`) and closes with
   4400. The server must choose from `hello.capabilities`.
3. `hello_ack.start: true` is a shortcut only when `control.owner` is `ws`:
   the device applies config, starts capture, and sends
   `capture_state: capturing`. With `control.owner: "mqtt"` it is ignored; the
   device sends `capture_state: idle` and waits for the MQTT `start`. Otherwise
   it sends `capture_state: idle` and waits for `start_capture`.
4. The server assigns a new `capture_id` for every capture start, including a
   reconnect. Segment messages echo it.
5. The effective `max_frame_bytes` is `min(device limit, server limit)`.
6. `telemetry.frame_stats: false` allows the device to skip per-frame
   RMS/peak/DC computation and clear `STATS_VALID`; periodic telemetry still
   runs.
7. Optional settings the device cannot honor are ignored and reported in
   `capture_state`; explicit commands for unsupported features return `nack`
   with `unsupported_feature`.
8. Format/rate/channel changes require idle capture. The active segment ends
   with `format_change`; a new capture is started.

### 5.3 Audio configuration object

| Field | Values / meaning |
|---|---|
| `format` | `pcm_s16le` or `opus` (v1). Reserved: `pcm_s24le`, `pcm_s32le`, `pcm_f32le`, `g711a`, `g711u`. |
| `sample_rate` | Hz. Must be in the device capability list. |
| `channels` | Interleaved channel count. |
| `frame_ms` | Nominal frame duration; Opus TOC is authoritative for actual packet duration. |
| `bits_per_sample` | 16 for `pcm_s16le`; informational for Opus. |
| `endianness` | `little` (required in v1). |
| `interleaved` | `true` (required in v1). |
| `opus.bitrate` | Requested bitrate; the device may ignore it if the encoder is chip-owned. |
| `opus.complexity` | 0–10; host encoders only. |
| `opus.vbr` | Host encoders only. |
| `opus.application` | `voip`, `audio`, or `restricted_lowdelay`; host encoders only. |
| `opus.encoder` | `gx8006` or `host`; MUST be one advertised by the device. |

GX8006 mic Opus is chip-owned: the server MUST NOT force bitrate or frame
duration, and the receiver MUST derive packet duration from the TOC.

### 5.4 Capture configuration object

| Field | Meaning |
|---|---|
| `mode` | `vad`, `continuous`, or `single`. |
| `pre_roll_ms` | Device-side pre-roll requested in `vad` mode; clamped to `device_pre_roll_max_ms`. |
| `max_segment_ms` | Force segment end. |
| `segment_ms` | Fixed segment length in `continuous` mode (default 10000). |
| `duration_ms` | Capture length in `single` mode. |
| `vad.engine` | `auto`, `energy`, `device`, or `server`. |
| `vad.onset_ms` | Consecutive speech required to confirm onset (default 120). |
| `vad.silence_ms` | Trailing non-speech that ends a segment (default 700). |
| `vad.min_speech_ms` | Speech that must accumulate before a segment opens (default 150). |
| `vad.guard_ms` | Frames ignored after a capture arm (default 0 for recorder). |
| `vad.threshold_db` | Energy VAD margin above the adaptive noise floor. |
| `vad.send_short_segments` | Send segments shorter than `min_speech_ms` instead of discarding them. |
| `gain_db` | Requested mic gain; unsupported platforms report `gain.supported: false`. |

---

## 6. Audio packet format

### 6.1 Header

Network byte order (big-endian). Total header size: **28 bytes**.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 1 | u8 | `version` | 1 |
| 1 | 1 | u8 | `type` | 1 = audio |
| 2 | 1 | u8 | `flags` | See §6.2 |
| 3 | 1 | u8 | `format` | 1 = PCM_S16LE, 2 = OPUS; 0 and 3+ reserved |
| 4 | 4 | u32be | `segment_id` | Segment identifier; JSON boundaries are authoritative |
| 8 | 4 | u32be | `seq` | 0-based within segment; increments for every captured frame, including dropped frames |
| 12 | 4 | u32be | `timestamp_ms` | Audio-clock ms of the first sample, relative to `capture_t0` |
| 16 | 2 | u16be | `samples` | Per-channel PCM samples in this frame (Opus: decoded duration) |
| 18 | 2 | u16be | `payload_len` | Payload bytes |
| 20 | 2 | i16be | `rms_dbfs_x100` | e.g. -3210 = -32.10 dBFS; `INT16_MIN` = unavailable |
| 22 | 2 | u16be | `peak` | Max abs sample normalized to S16 full scale; `0xFFFF` = unavailable |
| 24 | 2 | i16be | `dc_offset` | Mean sample in S16 LSB; `INT16_MIN` = unavailable |
| 26 | 1 | u8 | `vad_score` | 0–100; 255 = unknown |
| 27 | 1 | u8 | `reserved` | 0 |

### 6.2 Flags

| Bit | Mask | Name | Meaning |
|---:|---:|---|---|
| 0 | 0x01 | PRE_ROLL | Frame was captured before the speech onset that opened the segment. |
| 1 | 0x02 | SPEECH | The VAD decision for this frame is speech. |
| 2 | 0x04 | CLIPPED | One or more samples reached the clipping threshold. |
| 3 | 0x08 | DISCONTINUITY | A capture/send gap precedes this frame; seq may skip. |
| 4 | 0x10 | DTX/PLC | A gap was filled by codec DTX/PLC rather than real captured audio. |
| 5 | 0x20 | STATS_VALID | RMS/peak/DC/vad_score were computed from PCM and are valid. |
| 6–7 | 0xC0 | reserved | MUST be 0 in v1. |

There are intentionally no START/END flags. JSON `segment_start`/`segment_end`
are the sole boundary authority. A receiver may provisionally open a segment
when audio arrives first, but MUST NOT invent metadata; missing boundaries are a
protocol/connection-loss fault.

### 6.3 Field semantics

- **`seq`** resets to 0 at each `segment_start` and increments for every frame
  the capture path produces, including frames dropped before transmission.
  Therefore a missing `seq` means real audio loss. A receiver MUST NOT require
  contiguous sequence numbers; it records gaps.
- **`timestamp_ms`** is audio-clock time, derived from sample counts (PCM) or
  accumulated Opus TOC durations, relative to `capture_t0`. It is not wall clock
  and not arrival time. The 32-bit value wraps after ~49.7 days; the server
  unwraps it per segment. Pre-roll timestamps are earlier than
  `segment_start.onset_t_ms`.
- **`samples`** is per channel. For PCM,
  `payload_len == samples * channels * 2`. For Opus it is the decoded duration
  in samples; the server MUST use `samples / sample_rate` rather than the
  negotiated `frame_ms` when they differ.
- **Header stats** are computed from pre-encode PCM when available. For
  multichannel audio they are aggregate and normalized to S16 full scale: RMS
  across all channels, peak across all channels, mean DC. The server computes
  per-channel values from PCM. For chip-Opus pass-through, `STATS_VALID` is
  clear and the stats fields MUST be ignored.
- **`vad_score`** is 0–100 for energy/device VAD; 255 when unknown (for example
  chip Opus with no local decoder).
- **`rms_dbfs_x100`** uses 0.01 dB units. Values below –327.68 dB are clamped.
- **`peak`** is the maximum absolute sample normalized to 0..32768 for all
  formats. `32768` is valid full scale.
- **`dc_offset`** is the mean sample in S16 LSB, range –32767..32767;
  `INT16_MIN` is the unavailable sentinel.
- **`DISCONTINUITY`** marks a capture or TX-queue gap. A seq gap immediately
  before a DISCONTINUITY frame MUST NOT be counted as transport loss.
  **`DTX/PLC`** marks audio that was concealed rather than captured.

### 6.4 Validation

The server MUST close with 1002 if any of the following holds:

- Unknown `version` or `type`.
- Unknown `format` for this protocol version.
- `payload_len != len(message) - 28`.
- `28 + payload_len > effective max_frame_bytes`.
- PCM: `payload_len != samples * channels * 2`, or the format does not match
  the negotiated configuration.
- Opus: payload > 255 bytes, or a packet cannot be parsed at least to a TOC.

The device MUST NOT send a binary message larger than the negotiated limit and
MUST NOT truncate a payload to fit.

---

## 7. Segment semantics

### 7.1 Capture modes

| Mode | Device behavior | Typical use |
|---|---|---|
| `vad` | Segments speech with device-side VAD and pre-roll. | Linux/PCM; any platform with local PCM and energy/device VAD. |
| `continuous` | Sends every frame, split into fixed `segment_ms` blocks. VAD telemetry still runs when local PCM exists. | Mic pickup diagnosis; server VAD; LN882H Opus. |
| `single` | One segment of `duration_ms`, then stops capture. | Repeatable test captures. |

### 7.2 VAD engines

- **`energy`** (device): frame RMS against an adaptive noise floor with
  `threshold_db`; requires local PCM. Reuse the `VadEndpointTracker` +
  `EnergyVadFilter` behavior from ai-voice.
- **`device`** (chip): GX8006/ESP-SR START/STOP events. Controlled pre-roll may
  be unavailable; advertise `device_pre_roll_max_ms` accordingly.
- **`server`**: the device streams continuous Opus (or PCM); the server decodes,
  runs VAD, keeps pre-roll, and writes utterance files inside the capture. This
  is the required first-milestone LN882H path.
- **`auto`**: prefer `device` when the HAL reports utterance events; otherwise
  `energy` when local PCM exists; otherwise `server`.

### 7.3 Pre-roll and onset

- The device keeps the last `pre_roll_ms` of PCM in a ring.
- Onset requires `onset_ms` of consecutive speech decisions.
- A segment opens only after `min_speech_ms` of accumulated speech; a cough or
  key click does not open one.
- When the segment opens, the buffered pre-roll frames are sent first, flagged
  PRE_ROLL. The pre-roll ring must cover
  `pre_roll_ms + onset_ms + min_speech_ms`.
- `segment_start.t_ms` is the first frame actually sent (pre-roll start);
  `onset_t_ms` is the first frame whose VAD decision is speech; `pre_roll_ms`
  is the actual buffered duration. This lets the server measure the pre-roll
  noise floor separately from speech.
- A segment ends after `silence_ms` of trailing non-speech, or
  `max_segment_ms`, or `stop_capture`, `format_change`, `transport_lost`, or
  `queue_overflow`. Trailing silence frames are included in the segment.
- If `send_short_segments` is false, segments shorter than `min_speech_ms` are
  counted but not sent.

### 7.4 Server VAD and continuous capture

In `continuous` mode with `vad.engine: server`:

- The device sends one frame per capture frame and JSON segment boundaries only
  for file-size management (`segment_ms`) or explicit stop.
- The server decodes the stream, runs its own VAD, keeps its own pre-roll, and
  writes utterance WAVs inside the capture. Device-side VAD telemetry may be
  unavailable; the server does not depend on it.
- The device may still report chip START/STOP events as informational
  `telemetry`/`capture_state` details, but they do not define audio boundaries.

### 7.5 Segment end reasons

| Reason | Meaning |
|---|---|
| `silence` | Trailing silence exceeded the VAD window. |
| `max_duration` | `max_segment_ms` reached. |
| `stop_capture` | Server/operator stopped capture or started a new capture. |
| `format_change` | Audio format/rate/channels/gain changed; capture restarted. |
| `transport_lost` | WebSocket/session lost; segment closed locally. |
| `queue_overflow` | TX backpressure was severe; segment closed and restarted. |
| `error` | Microphone/chip/internal failure. |

---

## 8. JSON control messages

### 8.1 Envelope and correlation

Every message has a string `type`. Server commands MAY have a numeric `id`
(32-bit, monotonically increasing per connection). Device responses echo it.
Messages MAY include `session_id` and `capture_id`; the authoritative values are
those assigned by `hello_ack`.

Unknown fields MUST be ignored. Unknown message types get a `nack` with
`code: "unsupported_feature"` (or `bad_message` if malformed). A malformed JSON
message closes with 1002 after an `error` where possible.

### 8.2 Device → server messages

#### `hello`

Defined in §5.1. MUST be the first message.

#### `capture_state`

```json
{
  "type": "capture_state",
  "state": "capturing",
  "capture_id": "01J8Z4...",
  "mode": "vad",
  "audio": { "format": "pcm_s16le", "sample_rate": 16000, "channels": 1, "frame_ms": 20 },
  "capture": { "pre_roll_ms": 300, "vad_engine": "energy", "max_segment_ms": 15000 },
  "gain_db": 18,
  "gain_supported": true,
  "tx": { "queue_ms": 0, "drops": 0, "errors": 0, "reconnects": 0 }
}
```

`state`: `idle`, `capturing`, `paused`, or `error`. The device sends this after
applying `hello_ack`, after every successful `start_capture`/`stop_capture`, and
on error state changes.

#### `segment_start`

```json
{
  "type": "segment_start",
  "capture_id": "01J8Z4...",
  "segment_id": 12,
  "reason": "vad",
  "t_ms": 123400,
  "onset_t_ms": 123700,
  "pre_roll_ms": 300,
  "audio": { "format": "pcm_s16le", "sample_rate": 16000, "channels": 1, "frame_ms": 20 },
  "gain_db": 18,
  "label": "speech-01",
  "tone": null
}
```

`reason`: `vad`, `manual`, `continuous`, `single`, or `test_tone`. `tone`
metadata is present for test-tone segments (see §11). `label` is operator/test
metadata and is optional.

#### `segment_end`

```json
{
  "type": "segment_end",
  "capture_id": "01J8Z4...",
  "segment_id": 12,
  "reason": "silence",
  "t_ms": 126200,
  "frames": 140,
  "frames_dropped": 0,
  "pre_roll_frames": 15,
  "duration_ms": 2800,
  "speech_ms": 2100,
  "rms_dbfs": -28.4,
  "peak_dbfs": -3.1,
  "clip_pct": 0.0,
  "noise_dbfs": -61.2,
  "tx_drops": 0
}
```

`frames` is the number of frames produced for the segment; `frames_dropped` is
the number not sent. Network loss is not known to the device; the server derives
it from `seq` gaps.

#### `telemetry`

```json
{
  "type": "telemetry",
  "t_ms": 123456,
  "capture_id": "01J8Z4...",
  "vad_state": "silence",
  "vad_score": 8,
  "rms_dbfs": -62.1,
  "peak_dbfs": -45.0,
  "noise_dbfs": -64.3,
  "gain_db": 18,
  "tx_queue_ms": 0,
  "tx_drops": 0,
  "tx_errors": 0,
  "capture_overruns": 0,
  "reconnects": 0,
  "rssi_dbm": -55,
  "heap_free": 48000,
  "uptime_ms": 123456
}
```

`rssi_dbm` and `heap_free` are optional. `vad_state`: `speech`, `silence`, or
`unknown`. In server-VAD/Opus-only mode, device level fields may be `null`.

#### `stats` / `status`

Counters since boot or the last `reset_stats`: frames produced/sent/dropped,
bytes sent, TX queue high-water, send errors, capture overruns, reconnects,
segments produced/discarded, current capture/state/config. The server may show
these in the dashboard and report.

#### `tone_start` / `tone_end`

```json
{"type":"tone_start","tone_id":"thd-1k","t_ms":130000,"segment_id":20}
{"type":"tone_end","tone_id":"thd-1k","t_ms":135000,"segment_id":20,"reason":"completed"}
```

#### `accepted` / `completed` / `failed`

Long-command lifecycle for `test_tone` and future long commands:

```json
{"type":"accepted","id":42,"detail":"tone scheduled"}
{"type":"completed","id":42,"result":{"tone_id":"thd-1k","segment_id":20}}
{"type":"failed","id":42,"code":"audio_hw_error","detail":"speaker write failed"}
```

#### `pong`

```json
{"type":"pong","id":7,"t0_ms":123400,"t1_ms":123405,"server_time_ms":1760000000400}
```

For a device-initiated ping, `t0_ms` is the device send time; `t1_ms` is the
device receive time of the server pong. For a server-initiated ping, the device
echoes the server's timing fields and adds its own `t2_ms` if useful.

#### `ack` / `nack`

```json
{"type":"ack","id":7,"result":{}}
{"type":"nack","id":7,"code":"unsupported_feature","message":"set_gain unsupported","retryable":false,"detail":{}}
```

#### `error`

Asynchronous error:

```json
{"type":"error","code":"audio_hw_error","message":"mic init failed","fatal":true,"detail":{}}
```

### 8.3 Server → device messages

#### `hello_ack`

Defined in §5.2.

#### `configure`

```json
{
  "type": "configure",
  "id": 10,
  "audio": { "format": "pcm_s16le", "sample_rate": 48000, "channels": 2, "frame_ms": 10 },
  "capture": { "mode": "vad", "pre_roll_ms": 500, "vad": { "silence_ms": 900 } },
  "telemetry": { "frame_stats": true, "level_interval_ms": 250 }
}
```

Partial merge: omitted fields keep their current values. Format/rate/channel
changes require idle capture; the device ends the active segment with
`format_change`, applies the config, acks, and waits for `start_capture`. (Gain
changes use `set_gain`; the same idle/new-capture rule applies for clean
comparisons.)

#### `start_capture`

```json
{"type":"start_capture","id":11,"mode":"vad","label":"baseline","gain_db":18}
```

Optional `audio` overrides may be included instead of a separate `configure`.
The server assigns/uses a new `capture_id`. `duration_ms` applies to `single`
mode. When `control.owner` is `mqtt` and `allow_ws_capture_control` is false,
the device returns `nack` with `code: "control_owned_by_mqtt"`; MQTT is the
only start/stop authority.

#### `stop_capture`

```json
{"type":"stop_capture","id":12}
```

Ends the active segment with `stop_capture` and goes idle. Same
`control_owned_by_mqtt` rule as `start_capture`.

#### `set_gain`

```json
{"type":"set_gain","id":13,"gain_db":24}
```

Or `{"gain_raw": 4, "unit": "step"}` for native units. Ends the active segment
with `format_change` if the value changes; the server starts a new capture for a
clean comparison.

#### `set_rate`

```json
{"type":"set_rate","id":14,"sample_rate":48000}
```

Requires idle capture; starts a new capture on success.

#### `set_telemetry`

```json
{"type":"set_telemetry","id":15,"frame_stats":true,"level_interval_ms":500,"stats_interval_ms":5000}
```

#### `test_tone`

```json
{
  "type": "test_tone",
  "id": 16,
  "tone_id": "thd-1k",
  "waveform": "sine",
  "frequency_hz": 1000,
  "amplitude_dbfs": -6,
  "duration_ms": 5000,
  "fade_ms": 20,
  "speaker_volume": 60,
  "capture": true,
  "loopback": false,
  "label": "speaker-mic-loop"
}
```

- `waveform`: `sine`, `square`, or `sweep`.
- For `sweep`, use `start_hz`, `end_hz`, and `sweep_mode`
  (`linear`/`log`) instead of `frequency_hz`; `duration_ms` is the sweep time.
- `amplitude_dbfs` is relative to full scale before speaker volume.
- `capture: true` starts a `test_tone` segment simultaneously; the segment's
  `tone` metadata echoes the stimulus parameters.
- `loopback: true` requests an electrical/on-chip path; if unsupported, `nack`
  with `unsupported_feature`.
- The device responds `accepted`, then `tone_start`, `tone_end`, and
  `completed`/`failed`.

#### `ping`

```json
{"type":"ping","id":17,"t0_ms":123456}
```

The device replies `pong` immediately (bypassing the command queue). The server
may send pings without an `id`.

#### `get_stats` / `get_status` / `reset_stats`

```json
{"type":"get_stats","id":18}
{"type":"get_status","id":19}
{"type":"reset_stats","id":20}
```

The device replies with `ack` and a `result` object containing the counters or
current state. `get_status` is also useful after reconnect.

#### `play_audio`

Reserved for server-generated stimuli. v1 devices MUST reply `nack` with
`unsupported_feature`.

### 8.4 Error codes and close codes

Error codes (in `nack`/`error`):

| Code | Meaning |
|---|---|
| `bad_message` | Malformed JSON or missing required field. |
| `unsupported_version` | Protocol version not supported. |
| `unsupported_format` | Format/rate/channel/frame combination not advertised. |
| `unsupported_config` | Configuration incompatible with current state/platform. |
| `unsupported_feature` | Explicit feature not implemented (gain, loopback, play_audio). |
| `invalid_state` | Command not valid in the current state. |
| `busy` | Another long command or transition is in progress. |
| `capture_active` | Operation requires idle capture. |
| `capture_not_active` | Operation requires active capture. |
| `audio_hw_error` | Mic/speaker/HAL failure. |
| `chip_error` | Front-end co-processor failure (GX8006). |
| `queue_overflow` | TX queue overflow ended the operation. |
| `timeout` | Operation timed out. |
| `control_owned_by_mqtt` | The WS server tried to start/stop while MQTT owns control. |
| `control_conflict` | Concurrent/duplicate control command or ownership conflict. |
| `ws_unavailable` | Data-plane WebSocket could not connect/handshake. |
| `mqtt_unavailable` | Control plane is down; used in status/reporting. |
| `expired` | MQTT/WS command expired before execution. |
| `not_provisioned` | Missing MQTT/WS credentials. |
| `internal` | Unexpected internal error. |

WebSocket close codes:

| Code | Meaning |
|---|---|
| 1000 | Normal closure. |
| 1002 | Protocol error / malformed frame / invalid message. |
| 1008 | Policy violation. |
| 1011 | Server internal error. |
| 4400 | Bad hello / no supported configuration. |
| 4401 | Authentication failure. |
| 4408 | Hello timeout. |
| 4429 | Too many sessions (server policy). |

---

## 9. Telemetry and diagnostics

### 9.1 Per-frame telemetry

Carried in the binary header (§6). The server uses it to separate:

- **Transport/device loss:** seq gaps, DISCONTINUITY/DTX flags, arrival jitter.
- **Microphone pickup:** RMS/peak below the expected level, high noise floor,
  VAD never asserting.
- **Gain staging:** clipping (peak at full scale, CLIPPED), low level, high
  DC offset.
- **Codec/transport mismatch:** format/sample-count/payload-length anomalies.

### 9.2 Periodic telemetry

The `telemetry` JSON message (§8.2) is sent every `level_interval_ms`. It is the
only level signal while no segment is streaming, so it is essential for
"mic does not pick up" diagnosis. The server should display it live and log it
with the capture.

### 9.3 Device statistics

`stats`/`status` counters are lifetime or reset-since-boot values. The report
should include deltas per capture: frames produced/sent/dropped, bytes, TX
queue high-water, send errors, capture overruns, reconnects, segments
produced/discarded, UART/chip errors (LN882H), and heap high-water.

### 9.4 Server-computed metrics

The server MUST compute its own metrics from received PCM (or decoded Opus) and
MUST NOT trust device header stats for quality claims. See
`docs/audio-quality-metrics.md`. In particular:

- Opus captures are labeled lossy; THD+N/frequency-response results from Opus
  are not chip-grade.
- Server VAD and pre-roll are derived from decoded audio in `server` mode.
- Jitter/loss is computed on live frames; pre-roll arrives as a burst by design
  and is excluded from jitter but included in audio/sequence checks.

---

## 10. Timing and clock model

- `hello.clock.capture_t0_mono_us` is the device monotonic microsecond anchor at
  the start of the first capture (0 if capture has not started).
- Each frame's `timestamp_ms` is the audio-clock time of its first sample
  relative to `capture_t0`. For PCM it is derived from the sample counter; for
  Opus it is derived from accumulated TOC durations. It never uses LN882H's
  coarse wall clock.
- `hello.clock.wall_ms` and `synced` are informational for report timestamps.
  If the device has no NTP, `synced` is false and the server uses its own wall
  clock plus the clock probe.
- Clock probe: device `ping(t0)` → server records `t1` (receive), replies
  `pong(t0,t1,server_time)` → device records `t2`. The server computes
  offset/RTT and uses the minimum-RTT sample. The device may also send its own
  `ping` to the server. Audio timestamps remain audio-clock-relative.
- The server estimates clock skew by comparing audio-clock elapsed time to
  arrival elapsed time over long captures and reports it; it does not correct
  the audio by resampling unless explicitly configured.

---

## 11. Test tones and chip analysis

### 11.1 Test tone

- Generated locally on `HalAudioOut`/the platform speaker path, with
  phase-continuous sine/square or a log/linear sweep and `fade_ms` in/out.
- `speaker_volume` is explicit so the report can relate amplitude to the
  device's output setting.
- `capture: true` records the microphone (or loopback) while the tone plays.
- `segment_start.tone` metadata:

```json
{
  "tone_id": "thd-1k",
  "waveform": "sine",
  "frequency_hz": 1000,
  "amplitude_dbfs": -6,
  "speaker_volume": 60,
  "capture_path": "mic",
  "loopback": false,
  "fade_ms": 20
}
```

- On GX8006, speaker PCM chunk limits are unverified. The implementation must
  validate a safe chunk size and flow control before advertising `test_tone`;
  otherwise it may encode the tone to the chip's speaker Opus format or omit the
  capability.

### 11.2 Gain/rate sweeps

v1 uses **server orchestration**, not a firmware sweep state machine. For each
step, the server:

1. `stop_capture` (idle).
2. `configure` and/or `set_gain`/`set_rate`.
3. `start_capture` with `label` identifying the step.
4. Optionally `test_tone` with `capture: true`.
5. `stop_capture`.
6. Repeat.

Each step is a separate `capture_id` (or a labeled segment within one capture if
only gain changes), so its WAV/sidecar is self-describing. The server compares
gain linearity, clipping point, noise floor vs gain, THD+N vs rate, and
frequency response. A future firmware-side `sweep` convenience command can be
added without changing the base protocol.

### 11.3 Loopback and acoustic paths

- `loopback: false` means acoustic: speaker → room → microphone. This measures
  the complete audio path but not the codec alone.
- `loopback: true` requests an electrical/on-chip path; the device advertises
  `capabilities.loopback`. If unsupported, the server must use acoustic or
  external instrumentation.
- `capture_path` in the tone metadata documents `mic`, `line_in`, or `loopback`.
- For chip-grade THD+N/frequency response, use raw PCM and a known stimulus;
  document the measurement path in the report.

---

## 12. Flow control and backpressure

- All outbound device messages go through one bounded, frame-preserving TX
  queue. It contains control records (JSON) and audio records (binary).
- Control records (`segment_start`, `segment_end`, `ack`, `nack`, `accepted`,
  `completed`, `failed`, `error`, `pong`) are never dropped while the link is
  alive. If they cannot be sent, the device ends the capture and marks an error.
- Audio records may be dropped under pressure, oldest non-speech/pre-roll
  first. The device increments `frames_dropped`/`tx_drops`, marks the next sent
  frame DISCONTINUITY, and reports the counts in `segment_end`/`telemetry`.
- Capture never blocks on the socket. The transport writer runs on its own
  task/context.
- A server MUST continuously drain the socket and enforce per-connection
  message-size, session-count, and disk quotas. It MUST NOT let a slow disk
  writer stall the WebSocket read loop indefinitely; segment writes are
  buffered/queued.

---

## 13. Security

- Use `wss://` with CA verification by default. `ws://` is allowed only in a
  develop build gated by an explicit compile-time/runtime option.
- Require `Authorization: Bearer <device-token>`; the token is configured on the
  device and verified by the server. Do not send the token in JSON payloads.
- The device MUST NOT disable certificate verification in production. If a
  vendor SDK currently does so (as ai-voice's LN882H client does), patch/add CA
  verification before shipping.
- Do not commit Wi-Fi credentials, tokens, or CA private material. Use a
  gitignored generated header/template; compile-time Wi-Fi credentials are
  acceptable for the first LN882H milestone only when kept out of the repo.
- The server SHOULD rate-limit authentication failures and log the device
  identity and close reason.

---

## 14. Versioning and compatibility

- `protocol_version` is an integer; v1 = 1.
- Binary header `version` is 1; its size is fixed per version.
- Unknown JSON fields MUST be ignored. Unknown message types get
  `nack`/`unsupported_feature` when a response is possible.
- The server rejects an unsupported `protocol_version` with 4400.
- New optional fields/capabilities may be added in minor revisions without
  changing the header. A change to header layout, flag meanings, or required
  semantics requires a new major protocol version.
- Deprecation: a field/command is announced in docs and remains accepted for at
  least one minor version before removal.

---

## 15. Informative: server file layout and report contract

The server is not required to use this layout, but the reference Go server does:

```
data/captures/<capture_id>/
├── manifest.json              # device identity, config, session, timing, counts
├── summary.csv                # one row per segment with key metrics
├── report.html                # human-readable report with embedded plots
└── segments/<segment_id>/
    ├── meta.json              # segment_start/end, audio config, tone/sweep label
    ├── audio.wav              # decoded/raw audio in the negotiated format
    ├── packets.bin            # optional raw Opus packets, length-prefixed
    ├── frames.csv             # seq, device timestamp, arrival time, bytes, gaps
    ├── metrics.json           # server-computed metrics
    └── plots/*.png            # waveform, spectrum, spectrogram, etc.
```

Segment/audio files are per-capture; a reconnect always creates a new capture.
The report MUST state whether the capture was PCM or Opus, the measurement path
(mic/loopback), gain/rate settings, and any packet loss or device drops.

---

## 16. Worked examples

Unless noted, these assume MQTT has already issued the `start` command and
`control.owner` is `mqtt`; the WebSocket session is the data-plane handshake
that follows. See [`mqtt-control.md`](mqtt-control.md) for the control exchange.

### 16.1 PCM VAD segment (Linux)

1. Device hello (capabilities include `pcm_s16le`, `opus`, `vad: ["energy"]`).
2. Server hello_ack: PCM 16 kHz mono 20 ms, `vad` mode, pre-roll 300 ms,
   `start: true`, `capture_id: C1`.
3. Device capture_state: `capturing`, PCM config.
4. Speech detected; `segment_start`:
   `capture_id=C1, segment_id=1, reason=vad, t_ms=1000, onset_t_ms=1300,
   pre_roll_ms=300`.
5. 15 pre-roll frames + speech frames. First pre-roll frame header:
   `version=1, type=1, flags=0x21 (PRE_ROLL|STATS_VALID), format=1,
   segment_id=1, seq=0, timestamp_ms=1000, samples=320, payload_len=640,
   rms_dbfs_x100=-6120, peak=1200, dc_offset=3, vad_score=5`.
6. After 700 ms of silence, last frame then `segment_end`:
   `segment_id=1, reason=silence, frames=140, frames_dropped=0,
   duration_ms=2800, speech_ms=2100`.
7. Telemetry every 500 ms with levels/link/system.

### 16.2 Opus continuous/server VAD (LN882H)

1. Device hello: Opus-only, `vad: ["server","device"]`,
   `device_pre_roll_max_ms: 0`, `server_pre_roll: true`.
2. Server hello_ack: `format: opus`, 16 kHz mono 20 ms, `mode: continuous`,
   `vad.engine: server`, `start: true`, `capture_id: C2`.
3. Device sends continuous Opus frames; `STATS_VALID` is clear (chip Opus, no
   local decoder); `vad_score=255`.
4. Device sends `segment_start` for the capture window (`reason: continuous`,
   `pre_roll_ms: 0`) and boundaries every `segment_ms`; the server decodes,
   runs VAD, and writes utterance WAVs with its own pre-roll.
5. On stop: `segment_end` with `reason: stop_capture`; capture manifest lists
   all decoded utterances.

### 16.3 Test tone

1. Server sends `test_tone` with `waveform=sine`, `frequency_hz=1000`,
   `amplitude_dbfs=-6`, `duration_ms=5000`, `speaker_volume=60`,
   `capture=true`, `loopback=false`.
2. Device replies `accepted` (id), then `tone_start`, a `segment_start` with
   `reason=test_tone` and `tone` metadata, binary frames, `tone_end`,
   `segment_end`, `completed`.
3. Server computes frequency error, amplitude, THD, THD+N, SNR, and includes
   them in the report.

### 16.4 Gain sweep (server-orchestrated)

For gains `[0, 12, 24, 36]`:

```
stop_capture
configure {capture:{gain_db:0}} -> start_capture {label:"gain-0"} -> ...

stop_capture
configure {capture:{gain_db:12}} -> start_capture {label:"gain-12"} -> ...
```

Each step may play the same test tone. The report compares level, clipping,
noise floor, and SNR across labels.

---

## 17. Appendix: capability matrix template

Implementations should publish a filled matrix in `docs/porting.md`. The
template:

| Platform | PCM | Opus | Device VAD | Server VAD | Gain | Test tone | Loopback | Max frame |
|---|---|---|---|---|---|---|---|---|
| Linux (miniaudio) | yes | host | energy | yes | no | speaker | opt | 4096 B |
| LN882H + GX8006 | unproven | chip | chip | yes | chip | unverified | no | 2048 B |
| ESP32-S3 + ES8311 | yes | host | energy/ESP-SR | yes | yes | speaker | no | 4096 B |
| BK7258 | TBD | TBD | TBD | yes | TBD | TBD | TBD | TBD |
| AC791 | TBD | TBD | TBD | yes | TBD | TBD | TBD | TBD |
| macOS | yes | host | energy | yes | no | speaker | opt | 4096 B |
| WT01C202/C302 | TBD | TBD | TBD | yes | TBD | TBD | TBD | TBD |

“TBD” means the capability must be verified on hardware before the platform
advertises it. Do not copy ai-voice defaults into a recorder capability matrix
without testing.
