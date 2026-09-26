#!/usr/bin/env bash
# Host unit tests (no Pico needed): scripts run from anywhere, tests run from the repo root.
#   tests/run.sh            (CC=clang tests/run.sh to pick another compiler)
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
CFLAGS=(-std=c11 -Wall -Wextra -Werror -O1 -g -I src -I tests)
mkdir -p build-tests

"$CC" "${CFLAGS[@]}" src/rtl1_core.c tests/sha256_ref.c tests/test_rtl1.c -o build-tests/test_rtl1
"$CC" "${CFLAGS[@]}" src/ws_proto.c tests/test_ws.c -o build-tests/test_ws

status=0
for t in test_rtl1 test_ws; do
    echo "== $t"
    ./build-tests/$t || status=1
    echo
done
exit $status
