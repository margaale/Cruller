#!/usr/bin/env bash
# Local build. Paths default to this machine's setup; override them through the environment.
#   [CRULLER_VERSION=x.y.z] [PICO_BOARD=pico2_w] scripts/build.sh [Debug|Release]
set -euo pipefail
cd "$(dirname "$0")/.."

TOOLS_ROOT="${TOOLS_ROOT:-/c/Users/mArgAAle/pico}"
export PICO_SDK_PATH="${PICO_SDK_PATH:-$TOOLS_ROOT/pico-sdk}"
export FREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH:-$TOOLS_ROOT/FreeRTOS-Kernel}"
export PICO_TOOLCHAIN_PATH="${PICO_TOOLCHAIN_PATH:-$TOOLS_ROOT/tools/arm-gnu-14.2}"
# TinyUSB 0.21.0 instead of the SDK's 0.18.0: its rp2040 host runs bulk on EPX (not once-per-frame
# interrupt endpoints, ~62 KB/s, too slow for 2 Mbaud) and sizes the CDC RX FIFO buffer correctly.
export PICO_TINYUSB_PATH="${PICO_TINYUSB_PATH:-$TOOLS_ROOT/tinyusb-0.21.0}"
# Our TinyUSB patches, as one series on a clean checkout. A marker file holds the series' hash so
# an unchanged series is skipped (git apply can't check stacked patches that touch the same lines).
# Local edits to that TinyUSB checkout are discarded when the series changes.
series_hash=$(cat patches/tinyusb/*.patch | git hash-object --stdin)
marker="$PICO_TINYUSB_PATH/.cruller-patches"
if [ "$(cat "$marker" 2>/dev/null)" != "$series_hash" ]; then
    git -C "$PICO_TINYUSB_PATH" checkout -- .
    for p in patches/tinyusb/*.patch; do
        git -C "$PICO_TINYUSB_PATH" apply "$PWD/$p"
        echo "applied $p to $PICO_TINYUSB_PATH"
    done
    echo "$series_hash" > "$marker"
fi
PICOTOOL_DIR="${PICOTOOL_DIR:-$TOOLS_ROOT/tools/picotool/picotool}"
PIOASM_DIR="${PIOASM_DIR:-$TOOLS_ROOT/tools/sdk-tools/pioasm}"

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE="${1:-Release}" \
    ${CRULLER_VERSION:+-DCRULLER_VERSION="$CRULLER_VERSION"} \
    ${PICO_BOARD:+-DPICO_BOARD="$PICO_BOARD"} \
    -Dpicotool_DIR="$PICOTOOL_DIR" \
    -Dpioasm_DIR="$PIOASM_DIR"
ninja -C build
ls -l build/cruller.uf2 build/cruller_migration.bin
