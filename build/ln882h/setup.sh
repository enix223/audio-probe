#!/usr/bin/env bash
#
# Integrate the recorder into an LN882H SDK checkout.
#
# Usage:
#   build/ln882h/setup.sh [LN882H_SDK_PATH]
#
# The SDK path defaults to ../../liangniu/ln882h-sdk relative to this repo
# (sibling layout used by the workspace). This script only copies the project
# into the SDK and sets USER_PROJECT; it does not modify the recorder sources.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK="${1:-$ROOT/../../liangniu/ln882h-sdk}"

if [[ ! -f "$SDK/CMakeLists.txt" ]]; then
  echo "error: LN882H SDK not found at $SDK" >&2
  echo "usage: $0 [LN882H_SDK_PATH]" >&2
  exit 1
fi
if [[ ! -f "$ROOT/lib/embedded-iot-client/CMakeLists.txt" ]]; then
  echo "error: lib/embedded-iot-client is missing; run:" >&2
  echo "  git submodule update --init" >&2
  exit 1
fi

echo "recorder: $ROOT"
echo "iot-client: $ROOT/lib/embedded-iot-client"
echo "LN882H SDK: $SDK"

PROJECT="$SDK/project/recorder"
rm -rf "$PROJECT"
mkdir -p "$PROJECT"
cp -r "$ROOT/build/ln882h/project/recorder/." "$PROJECT/"

# Absolute path substitution used by the project CMake.
sed -i "s|/recorder|$ROOT|g" "$PROJECT/CMakeLists.txt"

# Point the SDK root at this project. Preserve a single active USER_PROJECT
# line; the SDK's own tree has a block of commented duplicates.
sed -i 's|^.*set(USER_PROJECT .*|    set(USER_PROJECT  recorder)|' "$SDK/CMakeLists.txt"

echo
echo "Setup complete. Configure/build with:"
echo "  export CROSS_TOOLCHAIN_ROOT=${CROSS_TOOLCHAIN_ROOT:-/usr}"
echo "  cmake -S \"$SDK\" -B \"$SDK/build-recorder\" -DUSER_PROJECT=recorder \\"
echo "    -DCMAKE_BUILD_TYPE=Release -DRECORDER_DIR=\"$ROOT\" \\"
echo "    -DRECORDER_WIFI_SSID=... -DRECORDER_WIFI_PASSWORD=... \\"
echo "    -DRECORDER_MQTT_ENDPOINT=... -DRECORDER_MQTT_DEVICE_ID=... \\"
echo "    -DRECORDER_MQTT_DEVICE_SECRET=... -DRECORDER_MQTT_CA_FILE=... \\"
echo "    -DRECORDER_WS_URL=... -DRECORDER_WS_CA_FILE=..."
echo "  cmake --build \"$SDK/build-recorder\" -j"
echo
echo "Credentials are compile-time values in the generated platform_config.h;"
echo "do not commit them. UART1 DMA requires CONFIG_IOT_UART_DMA_ENABLE=1 and"
echo "CONFIG_IOT_UART_DMA_RX_BUF_SIZE >= 2048 (already set in the template)."
