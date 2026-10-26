# Platform Porting Guide

The recorder core, protocol, segmenter, TX queue, and server are platform
independent. A port provides capture, optional speaker/test-tone output,
transport, clock, configuration, and a **verified capability descriptor**.

This guide lists what a platform must implement, the known platform notes, and
the checklist for adding a target without changing the protocol.

Status: Linux and LN882H are the first milestones. Other ai-voice platforms are
follow-on ports. Capabilities marked **TBD** must be bench-verified before being
advertised.

---

## 1. Platform contract

A platform adapter provides the following.

### 1.1 AudioSource

```cpp
struct CaptureFrame {
  const uint8_t* data;      // PCM bytes or Opus packet
  size_t bytes;             // exact length; never truncated
  size_t samples;           // per channel (PCM) or TOC-derived (Opus)
  uint32_t timestamp_ms;    // audio-clock time of first sample
  AudioFormat format;       // PCM_S16LE or OPUS
  SourceFlags flags;        // discontinuity, overflow, session start/stop
};

class AudioSource {
 public:
  virtual AvpErr Start() = 0;
  virtual AvpErr Stop() = 0;
  virtual AvpErr Configure(const AudioConfig& cfg) = 0;
  virtual AvpErr Read(CaptureFrame& out, uint32_t timeout_ms) = 0;
  virtual SourceStats Stats() const = 0;
  virtual ~AudioSource() = default;
};
```

Requirements:

- **Byte-exact** frames. Do not reinterpret Opus as `int16_t`; do not lose odd
  bytes; do not truncate when the caller's buffer is too small (queue the
  remainder).
- Sample/audio-clock timestamps derived from sample counts or Opus TOC, not
  wall clock.
- `Configure()` supports the advertised sample rate/channels/frame duration or
  returns `unsupported_config`.
- Gain changes are applied only between captures; `SetMicGain` semantics are
  platform-specific (dB, register step, or unsupported).
- Source events for device VAD/chip sessions, overflow, and discontinuity.
- No logging/allocation in the real-time callback.

### 1.2 MQTT control (required)

- Reuse embedded-iot-client's `AppInit`/`IoTCore`/`ThingModelContext`.
- Register the internal `recorderCmd` datapoint; report `recorderStatus`.
- MQTT start/stop is the control authority; WS start/stop is nacked unless
  `allow_ws_capture_control` is explicitly enabled.
- Publish status on connect, state changes, and command results; enforce
  `id` deduplication, `expires_at_ms`, and the MQTT-loss stop timeout.
- **No wake word/KWS.** Capture starts only on MQTT `start`.
- Provide MQTT credentials/CA in the platform config; never commit them.

### 1.3 Speaker/test-tone sink (optional)

- Write PCM (or the platform's encoded format) with a non-blocking or bounded
  call.
- Volume control if available.
- On co-processors that own the speaker format (GX8006), a frame-preserving
  speaker queue and a single feeder task.
- If no speaker or the path is unverified, omit `test_tone`/`speaker` from
  capabilities.

### 1.4 Transport

- WebSocket client with text/binary send and receive.
- One writer context only:
  - Linux/macOS: the libwebsockets service/writable context.
  - LN882H: a dedicated FreeRTOS task around the blocking lwIP/mbedTLS send.
  - ESP-IDF: `esp_websocket_client_send_bin` from the transport task.
  - BK7258/AC791: follow the vendor WS client's thread rules.
- TLS with CA verification; bearer token; optional develop-only `ws://`.
- Reconnect is driven by the recorder client (1 s → 30 s with jitter), not the
  transport.
- Never truncate a payload. Enforce the negotiated `max_frame_bytes`.

### 1.5 Clock and system services

- Audio-clock anchor `capture_t0_mono_us`.
- Monotonic time for timeouts/telemetry (uptime).
- Optional: heap free/min, RSSI, watchdog, reboot counter.

### 1.6 Config/storage

- Server URL, token, device id, default audio/gain settings.
- Wi-Fi credentials (compile-time for the first LN882H milestone, stored
  otherwise).
- Never commit credentials.

### 1.7 Capability descriptor

The platform must truthfully fill:

```json
{
  "formats": { "pcm_s16le": {...}, "opus": {...} },
  "vad": ["server", "energy", "device"],
  "gain": {"min":0,"max":42,"step":6,"unit":"dB"},
  "test_tone": {...},
  "speaker": {...},
  "loopback": false,
  "device_pre_roll_max_ms": 500,
  "server_pre_roll": true,
  "max_segment_ms": 30000,
  "tx_queue_ms": 3000,
  "limits": {"max_frame_bytes": 4096}
}
```

Omit features that are not implemented or verified. Never copy a capability
matrix from another platform/board without testing.

---

## 2. Platform matrix (initial)

| Platform | MQTT control | PCM | Opus | Device VAD | Server VAD | Gain | Test tone | Loopback | Max frame | Status |
|---|---|---|---|---|---|---|---|---|---|---|
| Linux (miniaudio) | yes (Paho) | yes | host | energy | yes | no | speaker | optional | 4096 B | first milestone |
| LN882H + GX8006 | yes (coreMQTT) | unproven | chip | chip | yes | chip | unverified | no | 2048 B | first milestone, Opus-only |
| macOS (miniaudio) | yes (Paho) | yes | host | energy | yes | no | speaker | optional | 4096 B | follow-on |
| ESP32-S3 + ES8311 | yes (esp-mqtt) | yes | host | energy/ESP-SR | yes | 0–42 dB | speaker | no | 4096 B | follow-on |
| ESP32-S3 + INMP441/MAX98357 | yes (esp-mqtt) | yes | host | energy | yes | no | speaker | no | 4096 B | follow-on |
| WT01C202/C302 (ESP32-C2/C3) | yes (esp-mqtt) | TBD | host | energy | yes | TBD | TBD | no | TBD | follow-on |
| BK7258 | yes (SDK MQTT) | TBD | TBD | TBD | yes | TBD | TBD | TBD | TBD | follow-on |
| AC791 | yes (SDK MQTT) | TBD | TBD | TBD | yes | TBD | TBD | TBD | TBD | follow-on |

Legend: **yes** = implemented and tested; **unproven** = protocol enum suggests
it but hardware evidence is missing; **TBD** = not yet verified; **host** =
device CPU encodes; **chip** = co-processor encodes. MQTT is required for
start/stop on every platform; the column names the transport used by
embedded-iot-client.

---

## 3. Porting checklist

1. **Audio capture**
   - [ ] Byte-exact `AudioSource` implemented; no truncation/odd-byte loss.
   - [ ] Sample rates/channels/frame durations verified.
   - [ ] Audio-clock timestamps derived from samples/TOC.
   - [ ] Gain behavior verified or capability omitted.
   - [ ] No allocation/logging in the callback.

2. **Transport**
   - [ ] WS text/binary send/receive works with the negotiated limits.
   - [ ] Single writer context; no send from the capture task.
   - [ ] TLS/CA works; token handshake works.
   - [ ] Reconnect/backoff integrated with the recorder client.
   - [ ] Payloads above the platform's old limit tested (no truncation).

3. **MQTT control**
   - [ ] `AppInit`/`IoTCore`/thing model wired; `recorderCmd` internal;
         `recorderStatus` reports.
   - [ ] MQTT start/stop controls capture; status on connect/state changes.
   - [ ] Duplicate/expired commands, MQTT-loss fail-safe, and ownership rules
         tested.
   - [ ] No wake word/KWS engine initialized.

4. **Segmenter**
   - [ ] Device VAD (energy/chip) works, or `server` mode is selected.
   - [ ] Pre-roll behavior matches the advertised max.
   - [ ] `seq` counts produced frames including drops.
   - [ ] `DISCONTINUITY`/`DTX` semantics honored.

5. **Telemetry**
   - [ ] Per-frame stats (PCM) or `STATS_VALID=0` (Opus pass-through).
   - [ ] Periodic telemetry (levels, queue, drops, reconnects).
   - [ ] Optional heap/RSSI.

6. **Test tone** (if advertised)
   - [ ] Tone generation, fades, volume.
   - [ ] Speaker path chunking/flow control.
   - [ ] Simultaneous capture works.
   - [ ] Loopback flag truthful.

7. **Security/config**
   - [ ] Credentials/token not in repo.
   - [ ] WSS/CA default; MQTT TLS/CA where supported; dev-only plaintext option.
   - [ ] Device id/boot id stable.

8. **Verification**
   - [ ] Protocol golden vectors pass on-device.
   - [ ] MQTT start/stop works against the chosen broker.
   - [ ] Linux server receives and decodes the platform's format.
   - [ ] 10-minute soak; no watchdog/heap failure.
   - [ ] Capability matrix updated with measured values.

---

## 4. Platform notes

### 4.1 Linux (first milestone)

- Capture: ai-voice `HalAudioInLinux`/miniaudio (ALSA/PulseAudio). Preallocate
  the read buffer; use `PcmFrameAccumulator`.
- Transport: libwebsockets. Patch the wrapper:
  - remove the 4096-byte truncation,
  - queue complete messages,
  - write only from the lws service/writable context.
- MQTT control: Paho MQTT + mbedTLS/OpenSSL, embedded-iot-client `AppInit`/
  `IoTCore`/thing model. No wake word.
- PCM + energy VAD + device pre-roll is the reference diagnostic path.
- Host Opus encoding via libopus.
- No hardware mic gain; `set_gain` returns `unsupported_feature`.
- Test tone via the default speaker; use ALSA loopback or an acoustic path.
- Storage: file-backed; CLI flags override defaults until `hello_ack`.

### 4.2 macOS

- Reuse the Linux miniaudio capture/playback and libwebsockets code; only the
  platform entry/config differ.
- Same PCM/energy-VAD behavior as Linux.
- Verify CoreAudio device selection and sample-rate behavior.

### 4.3 ESP32-S3 + ES8311

- Reuse ai-voice `HalAudioES8311` (I2S + `esp_codec_dev`), with a preallocated
  read buffer to avoid per-call PSRAM allocation.
- ES8311 ADC produces stereo; de-interleave to mono as ai-voice does; do not
  open with `channel=1` (RX can return zeros).
- Mic gain 0–42 dB in 6 dB steps; verify the register/`esp_codec_dev` path.
- MQTT control: `esp-mqtt` via embedded-iot-client `AppInit`/`IoTCore`/thing
  model. No WakeNet/KWS; capture starts on MQTT.
- Transport: `esp_websocket_client` with TLS; send from the transport task.
- Optional ESP-SR **VAD** (not wake word); for a mic-only recorder, energy VAD
  is cheaper and sufficient. If using ESP-SR, account for PSRAM/model partition
  cost. Do not enable WakeNet.
- Test tone via ES8311/PA; check volume caps.
- Cap max frame at 4096 B.

### 4.4 ESP32-S3 + INMP441/MAX98357

- I2S RX-only mic (INMP441) and I2S TX speaker (MAX98357); no codec gain.
- Preallocate I2S buffers; stereo slots with left-channel extraction as in
  ai-voice.
- Energy VAD + host Opus; test tone via MAX98357 (hardware GAIN pin, no
  software volume).

### 4.5 WT01C202/C302 (ESP32-C2/C3)

- ESP-IDF build; verify PSRAM/heap and whether hosting Opus is feasible.
- If host Opus is not feasible, advertise PCM only or chip/co-processor Opus if
  available.
- Verify the board's mic path and sample rate; TBD until tested.

### 4.6 BK7258

- Reuse ai-voice Beken HAL/WS. The three-core layout may place audio on a
  dedicated CPU; respect its threading/mailbox rules.
- Determine whether the mic path is PCM or a co-processor format; advertise only
  verified formats.
- WS/TLS memory and reconnect behavior must be measured.
- TBD: gain, test tone, loopback, max frame.

### 4.7 AC791 (Jieli)

- Reuse ai-voice Jieli HAL/WS. The audio subsystem may have its own buffering and
  format constraints.
- Verify PCM availability and sample rates; host Opus may be expensive.
- TBD: gain, test tone, loopback, max frame.

### 4.8 LN882H + GX8006

See [`ln882h.md`](ln882h.md). First milestone is Opus-only with server VAD;
PCM is enabled only after a bench test proves it. MQTT control is required
(coreMQTT via embedded-iot-client), with WebSocket on demand. Byte-exact
`Gx8006AudioSource`, UART DMA, continuous pickup soak, WSS/CA, and measured
heap/stacks are required. No KWS/wake word; capture starts on MQTT.

---

## 5. Adding a platform without protocol changes

1. Copy the platform HAL/WS from ai-voice into `third_party/upstream/` (or write
   a thin adapter) and record provenance.
2. Implement `AudioSource` with byte-exact frames and audio-clock timestamps.
3. Implement MQTT control via embedded-iot-client `AppInit`/`IoTCore`/thing
   model; register `recorderCmd`, report `recorderStatus`. No wake word.
4. Implement the transport writer wrapper with the platform's threading rules.
5. Implement clock/config/system adapters.
6. Fill the capability descriptor from measured facts.
7. Add the platform build target/entry (CMake, ESP-IDF component, SDK project).
8. Run the Linux/Go server against the platform; capture and inspect WAVs.
9. Test MQTT start/stop, duplicate/expired commands, and MQTT-loss fail-safe.
10. Update this matrix and `docs/verification.md`.
11. Do not add platform-specific branches to the protocol core; capability
    negotiation is the only mechanism.

---

## 6. Common pitfalls

- Reinterpreting Opus bytes as `int16_t` and losing odd bytes.
- Truncating a payload to a fixed buffer instead of queueing/segmenting.
- Sending from the capture task and stalling audio on a blocking socket.
- Using wall-clock timestamps (coarse/unsychronized) instead of audio-clock.
- Advertising a format/rate that the hardware cannot actually deliver.
- Assuming a co-processor's PCM enum means mic PCM (see GX8006).
- Enabling WakeNet/KWS when the recorder has no wake word; capture must start
  on MQTT.
- Letting both MQTT and WS start/stop without an ownership rule, causing
  conflicts or double starts.
- Forgetting MQTT command deduplication/expiry and the MQTT-loss fail-safe.
- Dropping pre-roll or the trailing silence tail.
- Counting `seq` only for sent frames, which makes device drops look like
  network loss.
- Forgetting to mark DISCONTINUITY after a drop.
- Leaving certificate verification disabled in a vendor WS client.
- Forgetting to re-arm a chip's mic/VAD after a reboot or speaker abort.
- Logging/allocating in a real-time audio callback.

---

## 7. Verification per platform

Minimum before declaring a port supported:

- Protocol golden vectors + a real capture on the target.
- A PCM capture (if advertised) and an Opus capture (if advertised) decoded by
  the reference server.
- Continuous capture with known speech/silence; verify levels, VAD, and
  pre-roll (or server VAD).
- Gain/rate settings (if advertised) applied and reflected in the report.
- Test tone/loopback (if advertised) measured and within tolerance.
- 10-minute soak with no watchdog resets or unbounded heap loss.
- Wi-Fi/WS reconnect test (AP restart and server restart).
- Tx queue overflow test: capture continues, drops counted, server sees
  discontinuities.
- Security: WSS/CA and token verified; credentials absent from the repo.
