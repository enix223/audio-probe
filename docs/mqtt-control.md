# MQTT Control Plane

The recorder uses **MQTT for control** and **WebSocket for audio/data**:

- **MQTT is always connected** once the device is provisioned. It carries
  start/stop commands, status, and optional low-rate telemetry.
- **WebSocket is the data plane.** It is connected on demand when recording
  starts (default) and carries the binary audio stream and detailed telemetry
  to the analysis server.
- There is **no wake word**. Capture starts when the MQTT `start` command
  arrives and stops on `stop`, `max_capture_ms`, MQTT-loss fail-safe, or a fatal
  error.

This document defines the MQTT control contract. The audio protocol is in
[`protocol.md`](protocol.md).

---

## 1. Why split control and data

| Concern | MQTT | WebSocket |
|---|---|---|
| Purpose | Control/status | Audio/telemetry |
| Connection lifetime | Always on | On demand while recording |
| Payload | Small JSON | JSON control + binary audio |
| Broker/cloud | embedded-iot-client IoT core | Recorder analysis server |
| Reliability | Persistent session/QoS | TCP ordered; no resume |
| Why | Remote start/stop must work while idle | High-rate audio needs a direct stream |

Keeping MQTT always on means the cloud/operator can start a recording even when
no WebSocket is open. Connecting the WebSocket only while recording saves RAM
and power, which matters on LN882H where MQTT TLS and WS TLS together are tight.

---

## 2. Composition: reuse embedded-iot-client like ai-voice

The recorder follows ai-voice's composition root:

1. Create a recorder-capable `PlatformFactory` that extends the
   embedded-iot-client factory with:
   - `AudioSource`/speaker HAL,
   - WebSocket client,
   - recorder-specific configuration.
2. Call `AppInit(factory)`. This starts:
   - storage/ConfigStore,
   - EventBus/SystemService/Network/NTP,
   - MQTT + `IoTCore` (OneNET/Azure/AWS/Aliyun/self-hosted),
   - `ThingModelContext`, `CommandHandler`, OTA (optional),
   - BLE provisioning (optional; built-in provisioner completes on MQTT
     connect), scheduler/threads.
3. Create the recorder app, register MQTT datapoints, and start it.
4. Do **not** create a wake-word engine. The audio source is opened and capture
   is gated by recorder state, not by KWS.

This is the same pattern as ai-voice's `VoiceAppInit()`, minus the voice
platform/wake word and with a recorder app instead of `Application`.

### 2.1 Datapoint registration

Use the existing internal-datapoint mechanism:

```cpp
// Downlink control property: the firmware handles it; never sent to an MCU.
IoTCore::RegisterInternalDatapoint("recorderCmd");

// Subscribe to parsed internal datapoints on the EventBus:
bus.Subscribe(kEventInternalDatapoint, [](const nonstd::any& payload) {
  const auto* named = nonstd::any_cast<NamedDatapoint>(&payload);
  // named->identifier == "recorderCmd", named->value is a string (JSON)
});
```

Status is reported upstream with the existing report event:

```cpp
bus.Publish(kEventDatapointReport,
            nonstd::any(NamedDatapoint{"recorderStatus", statusJsonString}));
```

`IoTCore` resolves the identifier through the thing model and publishes it using
the platform's normal upstream encoding (OneJSON property post or raw datapoint
frame).

---

## 3. MQTT backend mappings

### 3.1 OneJSON / thing-model platforms (primary)

Works with OneNET, Azure, AWS, Alibaba Cloud, and any backend that serves a
thing model and uses OneJSON property sets. This is the direct reuse of
ai-voice's IoT path.

Thing model properties (both strings, JSON payload):

```json
[
  { "i": 1, "n": "recorderCmd",    "t": 9, "l": 1024 },
  { "i": 2, "n": "recorderStatus", "t": 9, "l": 1024 }
]
```

- `i` = dpId, `n` = identifier, `t` = 9 (string), `l` = max byte length.
- Add `s` only if the backend's full-upload frame needs a positional sequence;
  the OneJSON property-set path does not.
- `recorderCmd` is registered as an internal datapoint. Inbound property set:
  ```json
  {
    "id": "42",
    "version": "1.0",
    "params": {
      "recorderCmd": "{\"v\":1,\"type\":\"recorder_cmd\",\"id\":\"c1\",\"action\":\"start\",\"params\":{\"mode\":\"vad\",\"label\":\"run-1\"}}"
    }
  }
  ```
  The IoT core parses the string property, resolves it through the thing model,
  and publishes `kEventInternalDatapoint` with identifier `recorderCmd`.
- Status report (built by the device, sent via `kEventDatapointReport`):
  ```json
  {
    "id": "1",
    "version": "1.0",
    "params": {
      "recorderStatus": "{\"v\":1,\"type\":\"recorder_status\",\"id\":\"c1\",\"state\":\"capturing\",\"capture_id\":\"01J8Z4...\"}"
    }
  }
  ```
- The cloud must set/define both properties in the product thing model; if the
  model is empty or lacks them, the device logs a warning and cannot route the
  command. Provide a compile-time fallback (`CONFIG_IOT_THING_MODEL_STRING`)
  when the backend does not serve a model.

### 3.2 Self-hosted / raw MQTT (optional)

For a local Mosquitto or a self-hosted broker, use the embedded-iot-client
self-hosted topics:

```
iot/{productId}/{deviceId}/raw/down    device subscribes
iot/{productId}/{deviceId}/raw/up      device publishes
```

The payload is the JSON envelope from §4/§5 directly (UTF-8), not nested in a
thing model.

**Important:** the stock `SelfHostedMqttIoTCore` forwards raw downlink payloads
to the MCU serial path, which the recorder does not have. The recorder needs a
small recorder-specific IoTCore/raw adapter that:

- subscribes to the raw downlink topic,
- parses the JSON command envelope,
- publishes it to the recorder app (not to a serial port),
- publishes status JSON to the raw uplink topic.

This adapter still reuses the embedded-iot-client `MQTTClient`, network,
storage, config, and threading; it only changes how inbound bytes are routed.

If a broker supports OneJSON/property sets, prefer §3.1 and avoid the adapter.

---

## 4. Command envelope

```json
{
  "v": 1,
  "type": "recorder_cmd",
  "id": "c1",
  "action": "start",
  "issued_at_ms": 1760000000000,
  "expires_at_ms": 1760000005000,
  "params": {}
}
```

| Field | Required | Meaning |
|---|---|---|
| `v` | yes | Envelope version, 1. |
| `type` | yes | `recorder_cmd`. |
| `id` | yes | Command id; echoed in status. String or number. |
| `action` | yes | `start`, `stop`, `configure`, `status`, `test_tone`. |
| `issued_at_ms` | optional | Wall-clock issuance time. |
| `expires_at_ms` | optional | Do not execute after this time. |
| `params` | optional | Action-specific object. |

### 4.1 `start`

```json
{
  "v": 1, "type": "recorder_cmd", "id": "c1", "action": "start",
  "params": {
    "mode": "vad",
    "label": "run-1",
    "duration_ms": 0,
    "audio": {
      "format": "pcm_s16le", "sample_rate": 16000,
      "channels": 1, "frame_ms": 20
    },
    "gain_db": 18,
    "pre_roll_ms": 300
  }
}
```

- `mode`: `vad`, `continuous`, or `single`.
- `audio`: optional overrides; must be in the device's verified capability
  matrix. The WebSocket server's `hello_ack` is still authoritative; if both
  specify a format, the WS server's negotiated config wins and the device
  reports any mismatch.
- `gain_db`: optional; applied before capture starts.
- `pre_roll_ms`: optional; clamped to the platform's
  `device_pre_roll_max_ms`.
- `label`: operator/test label, passed through to the WS `segment_start` and
  the report.
- The device connects the WebSocket if needed, performs `hello`/`hello_ack`,
  then starts capture. Status transitions:
  `idle → connecting_ws → handshaking → capturing`.
- If the WS server is unavailable, status is `error` with
  `code: "ws_unavailable"`; retry behavior is configured (see §7).

### 4.2 `stop`

```json
{
  "v": 1, "type": "recorder_cmd", "id": "c2", "action": "stop",
  "params": { "reason": "operator" }
}
```

- Stops capture, ends the active WS segment with `stop_capture`, flushes the
  final status, and transitions `capturing → draining → idle`.
- The WS connection may stay open for `ws_idle_timeout_ms` (default 10 s) to
  deliver final packets/telemetry, then closes if `ws_mode=on_demand`.
- Idempotent: stopping while idle returns the current idle status.

### 4.3 `configure`

```json
{
  "v": 1, "type": "recorder_cmd", "id": "c3", "action": "configure",
  "params": {
    "mode": "continuous",
    "audio": { "format": "opus", "sample_rate": 16000, "channels": 1, "frame_ms": 20 },
    "pre_roll_ms": 0,
    "gain_db": 18
  }
}
```

- Updates defaults for subsequent `start` commands.
- Requires idle capture; if capturing, return `nack`/status `error` with
  `code: "capture_active"` (or stop first).
- Does not itself start recording.

### 4.4 `status`

```json
{ "v": 1, "type": "recorder_cmd", "id": "c4", "action": "status" }
```

The device publishes the current status envelope (§5) with the same `id`.

### 4.5 `test_tone` (optional)

```json
{
  "v": 1, "type": "recorder_cmd", "id": "c5", "action": "test_tone",
  "params": {
    "waveform": "sine", "frequency_hz": 1000, "amplitude_dbfs": -6,
    "duration_ms": 5000, "speaker_volume": 60, "capture": true
  }
}
```

- Requires an active capture/WS session (or the device starts a temporary one
  if configured).
- The device emits `accepted`/`completed` status updates and the WS
  `test_tone` segment.
- This is optional; the primary requirement is `start`/`stop`.

### 4.6 Errors

| Code | Meaning |
|---|---|
| `bad_message` | Malformed envelope/params. |
| `unsupported_action` | Unknown `action`. |
| `unsupported_config` | Format/rate/mode not available. |
| `capture_active` | `configure`/operation requires idle. |
| `capture_not_active` | `stop`-only operation requires capture. |
| `ws_unavailable` | Could not connect/handshake the data plane. |
| `mqtt_unavailable` | Not applicable to inbound (device is receiving), but used in status when control is lost. |
| `expired` | `expires_at_ms` has passed. |
| `control_conflict` | Concurrent/duplicate command or WS control not allowed. |
| `audio_hw_error` | Mic/speaker failure. |
| `chip_error` | GX8006/front-end failure. |
| `not_provisioned` | Missing MQTT/WS credentials. |
| `internal` | Unexpected error. |

---

## 5. Status envelope

```json
{
  "v": 1,
  "type": "recorder_status",
  "id": "c1",
  "state": "capturing",
  "capture_id": "01J8Z4...",
  "segment_id": 12,
  "audio": {
    "format": "pcm_s16le", "sample_rate": 16000,
    "channels": 1, "frame_ms": 20
  },
  "gain_db": 18,
  "queue": { "tx_ms": 0, "drops": 0, "reconnects": 0 },
  "error": null,
  "ts_ms": 1760000000123
}
```

| Field | Meaning |
|---|---|
| `id` | Echoes the command id, or empty for an unsolicited state report. |
| `state` | `idle`, `connecting_ws`, `handshaking`, `capturing`, `draining`, `error`. |
| `capture_id` | Assigned by the WS `hello_ack`; empty while idle. |
| `segment_id` | Current/last segment. |
| `audio` | Effective format/rate/channels/frame. |
| `gain_db` | Effective gain, or null if unsupported. |
| `queue` | TX queue depth/drops/reconnects. |
| `error` | `{code, message, retryable}` or null. |
| `ts_ms` | Device wall/uptime-derived timestamp. |

Publish status:

- immediately after MQTT connects (current state),
- on every state transition,
- on command completion/failure,
- on WS disconnect/reconnect that affects recording,
- periodically if `status_interval_ms > 0` (default 0: events only).

Use QoS 1 where the backend supports it. The status property is a string, so
the device must escape/validate JSON and keep it within the thing-model length
(1024 bytes recommended).

Optional low-rate `recorder_telemetry` (levels, queue, heap) can be a third
string property; detailed per-frame telemetry stays on WebSocket.

---

## 6. State machine and WebSocket interaction

```
                 MQTT connected
                       │
                       ▼
                    idle ──start──► connecting_ws ──► handshaking ──► capturing
                       ▲                                                  │
                       │                                                  │
                       └──── draining ◄── stop / max_capture / mqtt_lost ─┘
                                                                     │
                                                                     ▼
                                                                  error
```

- `idle`: MQTT up; WS may be disconnected.
- `connecting_ws`: WS TCP/TLS connect.
- `handshaking`: hello/hello_ack, config applied.
- `capturing`: audio streaming.
- `draining`: stop requested; finalize the segment, flush, wait for the WS
  idle timeout.
- `error`: fatal audio/WS error; status includes code/retryable. MQTT remains
  connected.

### 6.1 Control ownership

MQTT is the authoritative control plane for start/stop:

- `hello` advertises `control: {"owner":"mqtt","allow_ws_capture_control":false}`.
- The WS server MAY still send `configure`, `test_tone`, `get_status`, `ping`.
- WS `start_capture`/`stop_capture` return `nack` with
  `code: "control_owned_by_mqtt"` unless the device/config enables
  `allow_ws_capture_control: true` (debug/test mode).
- If enabled, both sources feed the same serialized command queue; MQTT `stop`
  always wins over a concurrent WS start.
- The device reports the actual state on both MQTT status and WS
  `capture_state`, so the two planes never disagree about what is happening.

### 6.2 WebSocket connection policy

`ws_mode`:

- `on_demand` (default): connect on `start`, disconnect after
  `ws_idle_timeout_ms` following `stop`/error. Saves RAM/power on LN882H.
- `always`: connect at boot and keep it open; WS idles between recordings.

Even in `always` mode, the WS server is told that MQTT owns start/stop, so it
does not auto-start (`hello_ack.start` is ignored).

### 6.3 MQTT-loss fail-safe

If MQTT disconnects while capturing:

- Keep recording for `mqtt_lost_stop_timeout_ms` (default 30 s).
- If MQTT does not return within the timeout, stop capture, finalize the
  segment, close/drain WS, and enter `idle`/`error`.
- Publish the final status when MQTT reconnects.
- `max_capture_ms` (default 30 min) is enforced regardless of MQTT state.

This prevents an unbounded recording if the control plane is lost.

### 6.4 Start/stop command reliability

- `id` deduplication: a repeated `id` returns the last/current status without
  re-executing.
- `expires_at_ms`: expired commands return `expired` and do not start a
  recording. This avoids a stale queued command starting a session hours later.
- QoS 1 commands may be redelivered; deduplication covers it.
- `issued_at_ms` is for logging/report correlation; the device does not trust
  it for scheduling.
- If `start` arrives while already capturing: return the current capturing
  status (idempotent) unless `params.restart=true`, which stops and starts a
  new capture.
- If `stop` arrives while idle: return the idle status (idempotent).

---

## 7. Provisioning and credentials

- MQTT broker/credentials come from embedded-iot-client's ConfigStore:
  OneNET/Azure/AWS/Aliyun/self-hosted. The first LN882H milestone may use
  compile-time credentials in a gitignored generated header.
- Wi-Fi provisioning: because MQTT is now required, the built-in
  embedded-iot-client BLE provisioner (which completes on MQTT connect) is a
  natural fit. Compile-time Wi-Fi remains the first bring-up path; BLE can be
  enabled later.
- WebSocket server URL/token are recorder config, separate from MQTT.
- Never commit Wi-Fi/MQTT/WS credentials or CA private material.
- Use TLS for MQTT; use WSS for WS in production. On a trusted lab LAN, a
  develop-only build may use plain MQTT/WS to reduce LN882H memory pressure.
  Document which combination is in use in the report.

---

## 8. Resource and memory implications

MQTT is now always connected, so the earlier "skip MQTT to save RAM" plan no
longer applies.

- LN882H: MQTT TLS + WS TLS together may leave very little heap. Measure.
  Mitigations:
  - WS `on_demand` (default) — WS TLS is freed after stop.
  - Prefer MQTT TLS + `ws://` on a trusted lab LAN for bring-up; enable WSS
    after measuring.
  - Keep mbedTLS input buffers at their known-good size; tune lwIP pools and
    stacks only with measurements.
  - Keep the recorder app/AudioSource buffers lean; no AEC/wake word/decoder.
  - Enforce a bounded TX queue and stop capture on MQTT loss/max duration.
- Linux: the build now includes Paho MQTT + mbedTLS/OpenSSL + HTTP/thing model
  (or a self-hosted raw adapter), matching ai-voice's IoT stack.
- Other platforms: reuse embedded-iot-client's MQTT implementation; verify heap
  and TLS coexistence.

---

## 9. Security

- MQTT: TLS with CA verification where the backend supports it; otherwise a
  documented trusted-LAN exception. Unique device credentials; no shared
  secrets in the repo.
- MQTT command authorization: the broker/product should restrict who can
  publish `recorderCmd`; the device trusts the broker's authenticated session.
- WS: bearer token + WSS/CA as in [`protocol.md`](protocol.md).
- Status messages must not include credentials or audio.
- Rate-limit/ignore malformed commands; log ids and errors without secrets.
- Use `expires_at_ms` and `id` deduplication to resist replay/duplicate
  commands.

---

## 10. Testing

- MQTT connect/reconnect; status published on connect.
- `start` while WS down: transitions `connecting_ws`/`handshaking`/`capturing`
  or reports `ws_unavailable`.
- `stop`: segment finalized, status `idle`, WS closes after timeout.
- Duplicate `id`: no double start.
- Expired command: rejected.
- `start` while capturing: idempotent or restart per param.
- MQTT disconnect during capture: stops after timeout; status on reconnect.
- `max_capture_ms`: stops and reports.
- WS `start_capture` with MQTT control ownership: nacked.
- MQTT OneJSON property path and self-hosted raw path both tested.
- Thing model missing/wrong type: clear error/log, no crash.
- Broker restart, Wi-Fi restart, server restart: state converges and no
  unbounded recording.
- Memory: MQTT-only idle, MQTT+WS capturing, and post-stop WS teardown measured
  on LN882H.

See [`verification.md`](verification.md) for the full matrix.
