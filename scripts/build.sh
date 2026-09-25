#!/usr/bin/env bash
# Local build. Paths default to this machine's setup; override them through the environment.
#   [CRULLER_VERSION=x.y.z] scripts/build.sh [Debug|Release]
set -euo pipefail
cd "$(dirname "$0")/.."

TOOLS_ROOT="${TOOLS_ROOT:-/c/Users/mArgAAle/pico}"
export PICO_SDK_PATH="${PICO_SDK_PATH:-$TOOLS_ROOT/pico-sdk}"
export FREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH:-$TOOLS_ROOT/FreeRTOS-Kernel}"
export PICO_TOOLCHAIN_PATH="${PICO_TOOLCHAIN_PATH:-$TOOLS_ROOT/tools/arm-gnu-14.2}"
PICOTOOL_DIR="${PICOTOOL_DIR:-$TOOLS_ROOT/tools/picotool/picotool}"
PIOASM_DIR="${PIOASM_DIR:-$TOOLS_ROOT/tools/sdk-tools/pioasm}"

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE="${1:-Release}" \
    -Dpicotool_DIR="$PICOTOOL_DIR" \
    -Dpioasm_DIR="$PIOASM_DIR"
ninja -C build
ls -l build/cruller.uf2 build/cruller_migration.bin
