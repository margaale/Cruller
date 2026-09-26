#!/usr/bin/env bash
# Host unit tests (no Pico needed): scripts run from anywhere, tests run from the repo root.
#   tests/run.sh            (CC=clang tests/run.sh to pick another compiler)
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
mkdir -p build-tests
"$CC" -std=c11 -Wall -Wextra -Werror -O1 -g -I src -I tests \
    src/rtl1_core.c tests/sha256_ref.c tests/test_rtl1.c \
    -o build-tests/test_rtl1
./build-tests/test_rtl1
