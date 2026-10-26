# Vendoring and Upstream Provenance

The recorder is a new project that reuses selected code from `ai-voice` and
`embedded-iot-client`. It does **not** modify either upstream repository. This
document defines how upstream files are copied, recorded, patched, and verified.

Status: policy/design. The vendored tree does not exist yet.

---

## 1. Policy

1. **Copy, don't submodule.** Vendored files live under `third_party/upstream/`
   so the recorder builds without sibling repositories. Submodules can be added
   later if the project adopts git, but the initial design does not depend on
   them.
2. **Pin exact commits.** Every copied file is associated with a repository and
   commit. Never mix versions under one label.
3. **Preserve upstream-relative paths.** Includes such as
   `embedded-iot-client/src/core/utils/cl_logger.h` must continue to resolve.
4. **Record provenance.** `third_party/provenance.json` lists each file's
   repository, commit, source path, destination path, SHA-256, license
   reference, and patch status. `scripts/verify_provenance.sh` checks it in CI.
5. **Patch only when necessary.** Patches live under `third_party/patches/` with
   a note in `PATCHES.md` and a diff against the pinned upstream. Prefer writing
   new recorder code over modifying upstream code.
6. **Do not copy credentials.** No Wi-Fi passwords, tokens, CA private keys, or
   live `platform_config.h` files.
7. **Keep licenses.** Copy the upstream license/notice files relevant to the
   vendored subset and reference them in `provenance.json`.

---

## 2. Upstream pins

| Repository | Commit | Notes |
|---|---|---|
| `ai-voice` | `2f8818e` (2026-10-24, branch main) | Source for audio HAL, Opus, VAD, WS, Linux/LN882H platform code. |
| `embedded-iot-client` | `f787e55` (2026-10-19, v1.10.6/build 325) | Preferred standalone tree. |
| `embedded-iot-client` fallback | `b2b145b` (`v2.0.0-221`) | The ai-voice submodule snapshot. Use only if f787e55 has material API drift against the vendored ai-voice sources; document the choice. |

The ai-voice checkout vendors `embedded-iot-client` as a submodule at
`b2b145b`, while the standalone repository is at `f787e55`. Phase 0 must inspect
the newer tree and either adapt the copied ai-voice code or pin the older tree.
Never label a `b2b145b` copy as `f787e55`.

---

## 3. Vendored ai-voice files

Paths below are relative to the ai-voice repository. Destination paths mirror
the upstream relative layout under `third_party/upstream/ai-voice/`.

| Source | Destination | Purpose | Patch |
|---|---|---|---|
| `src/core/types/cl_types.h` | `src/core/types/cl_types.h` | `AvpErr`, audio types | none |
| `src/core/audio/cl_hal_audio.h` | `src/core/audio/cl_hal_audio.h` | HAL interfaces | none |
| `src/core/audio/cl_audio_frame.h` | `src/core/audio/cl_audio_frame.h` | Audio frame | none |
| `src/core/audio/cl_audio_filter.h` | `src/core/audio/cl_audio_filter.h` | Filter interface | none |
| `src/core/audio/cl_audio_pipeline.h` | `src/core/audio/cl_audio_pipeline.h` | Optional local pipeline | none |
| `src/core/audio/cl_pcm_accumulator.h` | `src/core/audio/cl_pcm_accumulator.h` | Fixed-frame accumulation | none |
| `src/core/audio/cl_audio_ring.h` | `src/core/audio/cl_audio_ring.h` | SPSC ring | none |
| `src/core/audio/cl_audio_codec.h` | `src/core/audio/cl_audio_codec.h` | Codec interface | none |
| `src/core/audio/cl_opus_codec.{h,cpp}` | same | Host Opus encode/decode | none |
| `src/core/audio/cl_opus_toc.h` | same | Opus TOC duration/validation | none |
| `src/core/audio/cl_vad_filter.h` | same | Endpoint tracker + energy VAD | optional getters; recorder may wrap instead |
| `src/core/audio/cl_pcm_level_meter.h` | same | Level reference | none (recorder writes its own telemetry) |
| `src/core/ws/cl_ws_client.h` | same | WS interface | none |
| `src/platforms/linux/audio/cl_hal_audio_linux.{h,cpp}` | same | miniaudio capture/playback | preallocated read buffer (recorder patch) |
| `src/platforms/linux/audio/cl_miniaudio_impl.c` | same | miniaudio backend | none |
| `src/platforms/linux/ws/cl_hal_ws_linux.{h,cpp}` | same | libwebsockets client | remove 4096 truncation; queue/write from lws context |
| `src/platforms/ln882h/protocol/cl_gx8006_protocol.{h,cpp}` | same | GX8006 framing/control | add/verify mic format + continuous controls as needed |
| `src/platforms/ln882h/audio/cl_gx8006_spk_queue.h` | same | Speaker frame queue reference | none |
| `src/platforms/ln882h/bsp/gx8006_fc_pin.{c,h}` | same | Flow-control GPIO | none |
| `src/platforms/ln882h/ws/cl_hal_ws_ln882h.{h,cpp}` | same | lwIP/mbedTLS WS | add CA verification; sender locking/queue |
| `tests/test_audio.cpp` / `tests/test_voice_platform.cpp` | `tests/upstream/` | Fake HAL/WS patterns | adapt, not compiled as-is |

The existing `src/platforms/ln882h/audio/cl_hal_audio_gx8006.{h,cpp}` is
**reference only**. Do not compile it unchanged; implement a recorder-specific
`Gx8006AudioSource` (§5.2).

---

## 4. Vendored embedded-iot-client subset

Use the standalone `f787e55` tree (or documented fallback). Vendor the whole
tree under `third_party/upstream/embedded-iot-client/` for path compatibility,
but compile only the needed sources:

### Core

- `cfgstore/cl_config_store.cpp`
- `eventbus/cl_eventbus.cpp`
- `sched/cl_task_scheduler.cpp` (if using the scheduler)
- `system/cl_system_service.cpp`
- `utils/cl_logger.cpp`
- `net/cl_network.cpp`
- `storage/cl_storage.h` (interface)
- `serial/cl_serial.h` (interface)
- `threading/*` interfaces
- `base64/cl_base64.cpp`
- `iot/cl_iot_core.cpp`, `iot/cl_onenet_iot_core.cpp`,
  `iot/cl_selfhosted_mqtt_iot_core.cpp`, plus the selected
  Azure/AWS/Alibaba core if that backend is used
- `iot/cl_device_token.cpp` (OneNET/self-hosted token)
- `thingmodel/cl_thing_model.cpp`
- `command/cl_command_handler.cpp`
- `frame/cl_linkjob_frame.cpp`
- `ntp/cl_ntp.cpp`
- `http/cl_http_client.cpp` (thing-model fetch where used)
- `crypto/cl_hmac_sha1_mbedtls.cpp`, `cl_sha256.cpp`, `cl_aes.cpp` as needed

### Linux platform

- `platform/linux/storage/file_storage.*`
- `platform/linux/threading/posix_*.{h,cpp}`
- `platform/linux/system/linux_system_service.*`
- `platform/linux/serial/posix_system_serial.*` (logging)
- `platform/linux/mqtt/paho_mqtt_client.*`
- `platform/linux/http/native_http_client.*` (thing model, if used)
- `platform/linux/ntp/posix_ntp.*`
- `platform/linux/ble/linux_ble.*` only if BLE provisioning is enabled
- `platform/linux/sched/linux_scheduler.*`
- A recorder-specific platform factory rather than the stock one if it drags in
  unused components; the MQTT/IoT core remains required.

### LN882H platform

- `platform/ln882h/storage/ln882h_storage.*`
- `platform/ln882h/net/ln882h_network.*`
- `platform/ln882h/system/ln882h_system_service.*`
- `platform/ln882h/threading/ln882h_*.{h,cpp}`
- `platform/ln882h/serial/ln882h_system_serial.*`,
  `ln882h_user_serial.*`
- `platform/ln882h/bsp/serial_hw.c`, `flash_bridge.c`
- `platform/ln882h/serial/serial_bridge.c`
- `platform/ln882h/mqtt/ln882h_mqtt_client.*`
- `platform/ln882h/http/ln882h_http_client.*` (thing model, if used)
- `platform/ln882h/ntp/ln882h_ntp.*`
- `platform/ln882h/ble/ln882h_ble.*` only if BLE provisioning is enabled
- `platform/ln882h/ota/*` only if OTA is enabled
- `platform/ln882h/config/proj_config.h` (template)
- `platform/ln882h/startup/startup_*.c`
- device MAC helper (`ln882h_device_mac.*`)

### Do not compile/start

OTA and BLE provisioning are optional; the normal build requires MQTT/IoT core
but not OTA/BLE. Do not compile voice-specific ai-voice components. The
recorder's `recorderCmd` is an internal datapoint and must never be routed to an
MCU serial path.

Keep interface headers for the services the recorder uses; the build must
explicitly list the compiled subset (no globbing).

---

## 5. Patches and forks

### 5.1 Linux WebSocket

Upstream `WsClientLinux::SendBinary` copies at most 4096 bytes and silently
truncates the rest; it also calls `lws_write` from the caller's thread. The
recorder patch:

- queue complete messages instead of truncating,
- write only from the libwebsockets service/writable context,
- return an error for oversize messages rather than sending a partial frame,
- add a test with a payload >4096 bytes.

### 5.2 GX8006 audio source (fork, not a patch)

Do not patch the voice-coupled `HalAudioGx8006` in place. Write a new
`Gx8006AudioSource` in the recorder:

- byte-exact frame queue (odd lengths preserved),
- configurable max frame ≥768 bytes,
- chip START/STOP as source events,
- mic format selection (Opus first; PCM only after bench proof),
- re-handshake after reboot,
- UART DMA and overrun counters.

The upstream HAL remains as reference.

### 5.3 LN882H MQTT/WebSocket security

Upstream ai-voice's LN882H WS client disables certificate verification
(`MBEDTLS_SSL_VERIFY_NONE`). The recorder must add CA verification, a bearer
token, and a develop-only opt-out. Apply the same rule to MQTT. Record the
patches and test WSS/MQTT-TLS against the Go server/broker with a self-signed
CA.

### 5.4 MQTT control adapter

The OneJSON/thing-model path needs no upstream patch:

- `IoTCore::RegisterInternalDatapoint("recorderCmd")` is public.
- `kEventInternalDatapoint` and `kEventDatapointReport` carry the command/status
  strings.
- The thing model defines `recorderCmd`/`recorderStatus` as string properties
  (type 9, length 1024).

For a self-hosted raw broker, the stock `SelfHostedMqttIoTCore` forwards raw
downlink bytes to the MCU serial path. The recorder implements a small adapter
that parses the JSON envelope and publishes status to the raw uplink topic; it
reuses the embedded-iot-client `MQTTClient`, network, storage, and threading.
Do not modify the upstream self-hosted core unless the adapter cannot be kept
local.

### 5.5 ESP32-S3 ES8311 read allocation (later)

Upstream `HalAudioES8311::Read` allocates a PSRAM buffer for reads larger than
128 stereo frames. The recorder port should preallocate a member buffer. Record
the patch when the ESP32-S3 port is implemented.

### 5.6 VAD getters (optional)

If the recorder reuses `EnergyVadFilter` directly, it may need a public
noise-floor/decision getter. Prefer wrapping it in recorder code; if patching,
keep it additive and documented.

---

## 6. External dependencies

| Dependency | Version | Use | Source |
|---|---|---|---|
| miniaudio | 0.11.21 | Linux/macOS capture/playback | FetchContent (pinned tag) |
| libopus | 1.5.2 | Host Opus encode/decode | FetchContent (pinned tag) or system |
| libwebsockets | v4.3.3 | Linux/macOS WebSocket | FetchContent (pinned tag) or system |
| Paho MQTT C | pinned via embedded-iot-client | Linux/macOS MQTT | FetchContent/system |
| mbedTLS / OpenSSL | platform SDK/system | MQTT/HTTP/WS TLS | system/SDK |
| coreMQTT | LN882H SDK | LN882H MQTT | SDK |
| esp-mqtt | ESP-IDF | ESP32 MQTT | SDK |
| cpp-httplib | embedded-iot-client vendored | Thing-model HTTP | vendored |
| Go `github.com/coder/websocket` | pinned in `go.mod` | Server WS | Go modules |
| Go `gonum.org/v1/gonum` | pinned | FFT/plots | Go modules |
| Go `gopkg.in/hraban/opus.v2` | pinned | Reference Opus decode (cgo) | libopus |
| Linux SDK | ESP-IDF 5.5.x, LN882H SDK, etc. | Platform builds | environment |

License files for each vendored/external dependency must be retained.

---

## 7. `provenance.json` format

```json
{
  "upstreams": {
    "ai-voice": {
      "repository": "https://.../ai-voice",
      "commit": "2f8818e",
      "license": "see upstream"
    },
    "embedded-iot-client": {
      "repository": "https://.../embedded-iot-client",
      "commit": "f787e55",
      "license": "see upstream"
    }
  },
  "files": [
    {
      "repository": "ai-voice",
      "commit": "2f8818e",
      "source": "src/core/audio/cl_opus_codec.cpp",
      "destination": "third_party/upstream/ai-voice/src/core/audio/cl_opus_codec.cpp",
      "sha256": "...",
      "patch": null,
      "license_ref": "ai-voice/LICENSE"
    },
    {
      "repository": "ai-voice",
      "commit": "2f8818e",
      "source": "src/platforms/linux/ws/cl_hal_ws_linux.cpp",
      "destination": "third_party/upstream/ai-voice/src/platforms/linux/ws/cl_hal_ws_linux.cpp",
      "sha256": "...",
      "patch": "third_party/patches/linux_ws_queue_and_no_truncate.patch",
      "license_ref": "ai-voice/LICENSE"
    }
  ]
}
```

`sha256` is the hash of the **destination file after patching** or the verbatim
copy? Choose one and document it. Recommended: hash the destination file and
record the upstream source hash separately:

```json
{
  "source_sha256": "...",
  "destination_sha256": "...",
  "patch": "..."
}
```

---

## 8. Scripts

### 8.1 `scripts/vendor_upstream.sh`

- Takes optional `AI_VOICE_DIR` / `IOT_CLIENT_DIR` paths (defaults to sibling
  repositories).
- Verifies the source commits match the pins.
- Copies the listed files into `third_party/upstream/...`.
- Writes/updates `provenance.json` with hashes.
- Does not modify the source repositories.
- Fails if a required source file is missing or a commit mismatches.

### 8.2 `scripts/verify_provenance.sh`

- Recomputes destination hashes and compares to `provenance.json`.
- Applies each patch to a temporary copy of the upstream source and verifies the
  destination matches.
- Fails if a vendored file is unrecorded or a patch is missing.
- Run in CI and before releases.

---

## 9. Updating upstream

1. Choose a new commit/tag.
2. Inspect diffs for the vendored files and the interfaces the recorder uses.
3. Update the pins in `provenance.json` and `docs/vendoring.md`.
4. Re-run `vendor_upstream.sh` and re-apply patches.
5. Run the protocol/unit tests and a Linux end-to-end capture.
6. Update patch notes and any adapted interfaces.
7. Bump the recorder's build metadata if behavior changed.

Do not update only one of ai-voice/embedded-iot-client without checking their
interface compatibility.

---

## 10. Licensing notes

- Retain upstream copyright/license notices in copied files and a top-level
  `third_party/README` mapping.
- Do not relicense upstream code.
- Opus has its own license/patent notes; libwebsockets/miniaudio likewise.
- The Go server's dependencies have their own licenses; `go mod vendor` (if
  used) must retain them.
