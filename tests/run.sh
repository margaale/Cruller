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
"$CC" "${CFLAGS[@]}" src/core/ota_fetch_proto.c tests/test_ota_fetch.c -o build-tests/test_ota_fetch
"$CC" "${CFLAGS[@]}" src/core/buttons.c tests/test_buttons.c -o build-tests/test_buttons
"$CC" "${CFLAGS[@]}" src/core/rt4k_info_core.c tests/test_rt4k_info.c -o build-tests/test_rt4k_info
"$CC" "${CFLAGS[@]}" src/core/json.c tests/test_json.c -o build-tests/test_json
# littlefs as the boards build it (its own warnings aside), under cfgfs over a flash of RAM
LFS=(-DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -I third_party/littlefs)
"$CC" -std=c11 -O1 -g "${LFS[@]}" -c third_party/littlefs/lfs.c -o build-tests/lfs.o
"$CC" -std=c11 -O1 -g "${LFS[@]}" -c third_party/littlefs/lfs_util.c -o build-tests/lfs_util.o
"$CC" "${CFLAGS[@]}" "${LFS[@]}" src/core/cfgfs.c tests/cfgfs_ram.c tests/test_cfgfs.c build-tests/lfs.o build-tests/lfs_util.o \
    -o build-tests/test_cfgfs
"$CC" "${CFLAGS[@]}" "${LFS[@]}" src/core/json.c src/core/gameid_core.c src/core/gameid.c src/core/cfgfs.c tests/cfgfs_ram.c \
    tests/test_gameid.c build-tests/lfs.o build-tests/lfs_util.o -o build-tests/test_gameid

status=0
for t in test_rtl1 test_ws test_power test_rfc2217 test_console test_svs test_ota_fetch test_buttons test_rt4k_info test_json test_cfgfs test_gameid; do
    echo "== $t"
    ./build-tests/$t || status=1
    echo
done
if command -v node >/dev/null; then
    echo "== page"
    node tests/check_page.js || status=1
    echo "== api (routes: docs/API.md, the page's)"
    node tests/check_api.js || status=1
    echo "== fw.js (RT4K firmware updater)"
    node tests/test_fw.js || status=1
    echo "== sd.js (SD card view)"
    node tests/test_sd.js || status=1
    echo "== profiles.js (profiles view)"
    node tests/test_profiles.js || status=1
    echo "== mapper.js (settings map)"
    node tests/test_mapper.js || status=1
    echo "== editor.js (profile editor)"
    node tests/test_editor.js || status=1
    echo "== gameid.js (gameID view)"
    node tests/test_gameid_page.js || status=1
fi
exit $status
