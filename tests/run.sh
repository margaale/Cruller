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
"$CC" "${CFLAGS[@]}" src/power_core.c tests/test_power.c -o build-tests/test_power
"$CC" "${CFLAGS[@]}" src/rfc2217_proto.c tests/test_rfc2217.c -o build-tests/test_rfc2217

status=0
for t in test_rtl1 test_ws test_power test_rfc2217; do
    echo "== $t"
    ./build-tests/$t || status=1
    echo
done
if command -v node >/dev/null; then
    echo "== page"
    node tests/check_page.js || status=1
    echo "== fw.js (RT4K firmware updater)"
    node tests/test_fw.js || status=1
    echo "== ui.js"
    node --check src/web/ui.js && echo "ui.js: syntax ok" || status=1
fi
exit $status
