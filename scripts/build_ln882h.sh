#!/usr/bin/env bash
#
# Configure and build the recorder for LN882H. Environment variables:
#   LN882H_SDK            SDK path (default: ../liangniu/ln882h-sdk)
#   CROSS_TOOLCHAIN_ROOT  toolchain root containing bin/arm-none-eabi-gcc
#                         (defaults to /usr when that compiler is present)
#   RECORDER_WIFI_SSID / RECORDER_WIFI_PASSWORD
#   RECORDER_MQTT_ENDPOINT / RECORDER_MQTT_PORT
#   RECORDER_MQTT_PRODUCT_ID / RECORDER_MQTT_DEVICE_ID / RECORDER_MQTT_DEVICE_SECRET
#   RECORDER_MQTT_CA_FILE / RECORDER_WS_URL / RECORDER_WS_TOKEN / RECORDER_WS_CA_FILE
#   RECORDER_WS_INSECURE (0/1)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${LN882H_SDK:-$ROOT/../../liangniu/ln882h-sdk}"
export CROSS_TOOLCHAIN_ROOT="${CROSS_TOOLCHAIN_ROOT:-/usr}"
BUILD="$SDK/build-recorder"

"$ROOT/build/ln882h/setup.sh" "$SDK"

ARGS=(
  -S "$SDK" -B "$BUILD"
  -DUSER_PROJECT=recorder
  -DCMAKE_BUILD_TYPE=Release
  -DRECORDER_DIR="$ROOT"
  -DRECORDER_WIFI_SSID="${RECORDER_WIFI_SSID:-}"
  -DRECORDER_WIFI_PASSWORD="${RECORDER_WIFI_PASSWORD:-}"
  -DRECORDER_MQTT_ENDPOINT="${RECORDER_MQTT_ENDPOINT:-127.0.0.1}"
  -DRECORDER_MQTT_PORT="${RECORDER_MQTT_PORT:-8883}"
  -DRECORDER_MQTT_PRODUCT_ID="${RECORDER_MQTT_PRODUCT_ID:-recorder-lab}"
  -DRECORDER_MQTT_DEVICE_ID="${RECORDER_MQTT_DEVICE_ID:-recorder-ln882h}"
  -DRECORDER_MQTT_DEVICE_SECRET="${RECORDER_MQTT_DEVICE_SECRET:-}"
  -DRECORDER_MQTT_CA_FILE="${RECORDER_MQTT_CA_FILE:-}"
  -DRECORDER_WS_URL="${RECORDER_WS_URL:-ws://192.168.1.100:8080/v1/recorder}"
  -DRECORDER_WS_TOKEN="${RECORDER_WS_TOKEN:-}"
  -DRECORDER_WS_CA_FILE="${RECORDER_WS_CA_FILE:-}"
  -DRECORDER_WS_INSECURE="${RECORDER_WS_INSECURE:-0}"
)

cmake "${ARGS[@]}"
cmake --build "$BUILD" -j"$(( $(nproc) > 8 ? 8 : $(nproc) ))"
echo "built: $BUILD/bin/recorder.elf"
