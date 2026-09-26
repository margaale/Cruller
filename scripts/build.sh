#!/usr/bin/env bash
# Local build. Paths default to this machine's setup; override them through the environment.
#   [CRULLER_VERSION=x.y.z] scripts/build.sh [Debug|Release]
set -euo pipefail
cd "$(dirname "$0")/.."

TOOLS_ROOT="${TOOLS_ROOT:-/c/Users/mArgAAle/pico}"
export PICO_SDK_PATH="${PICO_SDK_PATH:-$TOOLS_ROOT/pico-sdk}"
export FREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH:-$TOOLS_ROOT/FreeRTOS-Kernel}"
export PICO_TOOLCHAIN_PATH="${PICO_TOOLCHAIN_PATH:-$TOOLS_ROOT/tools/arm-gnu-14.2}"
# TinyUSB 0.21.0 instead of the SDK's 0.18.0: its rp2040 host runs bulk on EPX (not once-per-frame
# interrupt endpoints, ~62 KB/s, too slow for 2 Mbaud) and sizes the CDC RX FIFO buffer correctly.
export PICO_TINYUSB_PATH="${PICO_TINYUSB_PATH:-$TOOLS_ROOT/tinyusb-0.21.0}"
# Our TinyUSB patches (idempotent: skipped once applied).
for p in patches/tinyusb/*.patch; do
    if git -C "$PICO_TINYUSB_PATH" apply --reverse --check "$PWD/$p" 2>/dev/null; then continue; fi
    git -C "$PICO_TINYUSB_PATH" apply "$PWD/$p"
    echo "applied $p to $PICO_TINYUSB_PATH"
done
PICOTOOL_DIR="${PICOTOOL_DIR:-$TOOLS_ROOT/tools/picotool/picotool}"
PIOASM_DIR="${PIOASM_DIR:-$TOOLS_ROOT/tools/sdk-tools/pioasm}"

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE="${1:-Release}" \
    ${CRULLER_VERSION:+-DCRULLER_VERSION="$CRULLER_VERSION"} \
    -Dpicotool_DIR="$PICOTOOL_DIR" \
    -Dpioasm_DIR="$PIOASM_DIR"
ninja -C build
ls -l build/cruller.uf2 build/cruller_migration.bin
