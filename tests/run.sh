#!/usr/bin/env bash
# Host unit tests (no Pico needed): scripts run from anywhere, tests run from the repo root.
#   tests/run.sh            (CC=clang tests/run.sh to pick another compiler)
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
CFLAGS=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -I src/core -I tests)
mkdir -p build-tests

"$CC" "${CFLAGS[@]}" src/core/rtl1_core.c tests/sha256_ref.c tests/test_rtl1.c -o build-tests/test_rtl1
"$CC" "${CFLAGS[@]}" src/core/ws_proto.c tests/test_ws.c -o build-tests/test_ws
"$CC" "${CFLAGS[@]}" src/core/power_core.c tests/test_power.c -o build-tests/test_power
"$CC" "${CFLAGS[@]}" src/core/rfc2217_proto.c tests/test_rfc2217.c -o build-tests/test_rfc2217
"$CC" "${CFLAGS[@]}" src/core/console_core.c tests/test_console.c -o build-tests/test_console
"$CC" "${CFLAGS[@]}" src/core/svs_proto.c tests/test_svs.c -o build-tests/test_svs

status=0
for t in test_rtl1 test_ws test_power test_rfc2217 test_console test_svs; do
    echo "== $t"
    ./build-tests/$t || status=1
    echo
done
if command -v node >/dev/null; then
    echo "== page"
    node tests/check_page.js || status=1
    echo "== fw.js (RT4K firmware updater)"
    node tests/test_fw.js || status=1
fi
exit $status
