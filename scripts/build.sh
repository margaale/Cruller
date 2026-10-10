#!/usr/bin/env bash
# Local build of a target (src/platform/<target>) into build/<target>. Paths default to this
# machine's setup; override them through the environment.
#   [CRULLER_VERSION=x.y.z] [CRULLER_BUILD=n] [CRULLER_DEBUG=0] [PICO_BOARD=pico2_w] scripts/build.sh [rp2] [Debug|Release]
#   (CRULLER_DEBUG=0: without the developer tools, see src/platform/platform.h)
#   [CRULLER_VERSION=x.y.z] [CRULLER_DEBUG=0] scripts/build.sh esp32   (in an ESP-IDF 6.1 shell: idf.py on the PATH)
set -euo pipefail
cd "$(dirname "$0")/.."

target=rp2
case "${1:-}" in rp2|esp32) target=$1; shift ;; esac
build_type="${1:-Release}"
src="src/platform/$target"
out="build/$target"

if [ "$target" = esp32 ]; then
    # Optimization comes from sdkconfig.defaults, not a build type.
    idf.py -C "$src" -B "$out" ${CRULLER_VERSION:+-DCRULLER_VERSION="$CRULLER_VERSION"} \
        -DCRULLER_DEBUG="${CRULLER_DEBUG:-1}" reconfigure
    ninja -C "$out" -k 0 # every error at once, not just the first
    # A new board over USB: bootloader, partition table, OTA data and app in one file, at 0x0.
    idf.py -C "$src" -B "$out" merge-bin -o "$PWD/$out/cruller-factory.bin"
    ls -l "$out/cruller.bin" "$out/cruller-factory.bin"
    exit 0
fi

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
series_hash=$(cat $src/patches/tinyusb/*.patch | git hash-object --stdin)
marker="$PICO_TINYUSB_PATH/.cruller-patches"
if [ "$(cat "$marker" 2>/dev/null)" != "$series_hash" ]; then
    git -C "$PICO_TINYUSB_PATH" checkout -- .
    for p in $src/patches/tinyusb/*.patch; do
        git -C "$PICO_TINYUSB_PATH" apply "$PWD/$p"
        echo "applied $p to $PICO_TINYUSB_PATH"
    done
    echo "$series_hash" > "$marker"
fi
PICOTOOL_DIR="${PICOTOOL_DIR:-$TOOLS_ROOT/tools/picotool/picotool}"
PIOASM_DIR="${PIOASM_DIR:-$TOOLS_ROOT/tools/sdk-tools/pioasm}"

cmake -S "$src" -B "$out" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" \
    ${CRULLER_VERSION:+-DCRULLER_VERSION="$CRULLER_VERSION"} \
    ${CRULLER_BUILD:+-DCRULLER_BUILD="$CRULLER_BUILD"} \
    -DCRULLER_DEBUG="${CRULLER_DEBUG:-1}" \
    ${PICO_BOARD:+-DPICO_BOARD="$PICO_BOARD"} \
    -Dpicotool_DIR="$PICOTOOL_DIR" \
    -Dpioasm_DIR="$PIOASM_DIR"
ninja -C "$out"
ls -l "$out/cruller.uf2" "$out/cruller-factory.uf2" "$out/cruller_migration.bin"
