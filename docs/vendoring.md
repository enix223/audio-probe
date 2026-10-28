# Dependencies and Submodules

Status: implemented. This document supersedes the earlier copy-based
`third_party/upstream` policy.

---

## 1. Policy

1. **Dependencies are git submodules or pinned FetchContent packages.**
   - `lib/embedded-iot-client` — submodule for MQTT/IoT control.
   - C/C++ libraries — CMake `FetchContent` with pinned tags:
     miniaudio 0.11.21, libopus 1.5.2, libwebsockets 4.3.3.
   - embedded-iot-client's own CMake fetches mbedTLS 3.6.0 and Paho MQTT C
     1.3.13.
2. **Do not modify the submodule.** Recorder-specific behavior lives in
   `src/core/` and `src/platforms/`.
3. **No ai-voice dependency.** The recorder implements the small pieces it
   needs (Linux miniaudio source/sink, Linux libwebsockets client, GX8006
   protocol/source, endpoint tracker, Opus wrapper) itself. The former plan to
   vendor selected ai-voice files is retired.
4. **No credentials.** The submodule and the repository contain no Wi-Fi,
   MQTT, WebSocket, or CA private material. Credentials are CMake variables
   baked into a generated `platform_config.h` (Linux/LN882H) or supplied on the
   device command line.

## 2. Submodule pin

| Repository | Commit | Notes |
|---|---|---|
| `embedded-iot-client` | `b2b145b` (v2.0.0-221) | Required internal-datapoint APIs. |
| preferred `f787e55` | rejected | v1.10.6 lacks `RegisterInternalDatapoint`, `kEventInternalDatapoint`, `kEventDatapointReport`, and `NamedDatapoint`. Using it would silently route `recorderCmd` to the MCU serial path or fail to compile the control adapter. |
| `ai-voice` | not used | No code, no submodule, no build dependency. |

`b2b145b` is the embedded-iot-client commit that the pinned ai-voice snapshot
vendored, so its APIs match the composition the recorder's control adapter was
written against. The fallback is deliberate and documented, per the original
vendoring policy's API-drift rule.

Initialize the submodule and its nested libraries:

```bash
git submodule update --init --recursive
git submodule status --recursive
```

Nested submodules under `lib/embedded-iot-client/lib/`:

- `rapidjson` (Tencent/rapidjson)
- `cpp-httplib` (yhirose/cpp-httplib)
- `nonstd/any-lite`, `optional-lite`, `variant-lite`

Their `.gitmodules` URLs are SSH in the upstream repository. If SSH is not
available, override them locally (do not commit the override):

```bash
cd lib/embedded-iot-client
git config submodule.lib/rapidjson.url https://github.com/Tencent/rapidjson.git
git config submodule.lib/cpp-httplib.url https://github.com/yhirose/cpp-httplib.git
git config submodule.lib/nonstd/any-lite.url https://github.com/nonstd-lite/any-lite.git
git config submodule.lib/nonstd/optional-lite.url https://github.com/nonstd-lite/optional-lite.git
git config submodule.lib/nonstd/variant-lite.url https://github.com/nonstd-lite/variant-lite.git
git -c protocol.file.allow=always submodule update --init
```

`protocol.file.allow=always` is only needed when cloning from a local mirror.

## 3. What is compiled from embedded-iot-client

Linux (`build/nix/CMakeLists.txt`):

- `core` (all `src/core/*.cpp`, gated by the generated platform config)
- `recorder-iot-platform` (Linux platform sources):
  storage, threading, system, serial, Paho MQTT, native HTTP, NTP, scheduler,
  network, OTA PAL, platform factory.

LN882H (`build/ln882h/project/recorder/CMakeLists.txt`):

- MQTT/IoT core, thing model, command handler, storage/config/eventbus/
  scheduler/system/network/NTP/HTTP, plus the LN882H platform adapters.
- BLE provisioning and OTA manager are not compiled; the LN882H BLE stack is
  still linked because the SDK's factory references it.

The recorder registers `recorderCmd` with `IoTCore::RegisterInternalDatapoint`
and reports `recorderStatus` through `kEventDatapointReport`. The stock
self-hosted raw core's serial routing is not used.

## 4. Reproducing the build

```bash
# Linux
cmake -S . -B out/nix -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/nix --target recorder recorder-tests -j

# LN882H
export CROSS_TOOLCHAIN_ROOT=/usr   # or the official ARM toolchain root
./build/ln882h/setup.sh
./scripts/build_ln882h.sh
```

The LN882H setup copies `build/ln882h/project/recorder` into
`$LN882H_SDK/project/recorder`, substitutes the absolute repository path, and
sets `USER_PROJECT=recorder`. It does not modify the recorder sources.

## 5. Toolchain note (LN882H)

The Ubuntu `arm-none-eabi` multilib does not provide `__aeabi_read_tp`, while
libstdc++ may reference it from the exception/TLS path. The recorder supplies a
weak 256-byte thread-pointer stub in
`src/platforms/ln882h/bsp/cl_arm_tls_stub.c`; the official ARM toolchain's
strong definition wins when present. This is a build workaround for the host
toolchain, not a hardware-verified capability. Prefer the official ARM GNU
toolchain for production images and re-check the exception/TLS behavior.

## 6. Updating dependencies

1. Choose a new submodule commit/tag. Inspect the internal-datapoint, thing
   model, and MQTT APIs first.
2. Update the pin in this document and `.gitmodules`; run the Linux and LN882H
   builds.
3. Run `git submodule update --init --recursive` and both test suites.
4. Do not update only one of the recorder and the submodule without checking
   the control-adapter interface.

## 7. Licenses

- Retain the submodule's license/notice files (`lib/embedded-iot-client`).
- External libraries keep their upstream licenses: miniaudio, libopus,
  libwebsockets, mbedTLS, Paho MQTT C, rapidjson, cpp-httplib, nonstd.
- The Go server dependencies are recorded in `server/go.mod`/`go.sum`; Go's
  module cache retains their licenses.
