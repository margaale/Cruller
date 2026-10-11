#!/usr/bin/env bash
# Host tests (no board needed): src/core's pure modules in C, and the page's JavaScript with node. Builds
# and tests run side by side; each one's output comes at the end, in order.
#   tests/run.sh          all of them (CC=clang tests/run.sh to pick another compiler)
#   tests/run.sh --page   the page's only: CI runs them again on the minified page
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-tests
status=0

# The page's: check_page (each id app.js uses is in index.html), check_api (the routes: docs/API.md's, the
# page's, http.c's; it reads the sources, so not on the minified page), and each test_*.js.
page() {
    local t s=0 list=(tests/check_page.js)
    [ "$1" = all ] && list+=(tests/check_api.js)
    for t in "${list[@]}" tests/test_*.js; do
        echo "== $t"
        node "$t" || s=1
    done
    return $s
}
if ! command -v node > /dev/null; then
    echo "(no node: the page's tests left out)"
    page() { :; }
fi
if [ "${1:-}" = --page ]; then
    page page
    exit
fi
page all > build-tests/page.log 2>&1 &
page_pid=$!

CC="${CC:-gcc}"
CFLAGS=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -I src/core -I tests)
tests=() pids=()
# tests/<name>.c with the sources it tests, built in the background.
build() {
    local name=$1
    shift
    "$CC" "${CFLAGS[@]}" "$@" "tests/$name.c" -o "build-tests/$name" > "build-tests/$name.log" 2>&1 &
    tests+=("$name")
    pids+=($!)
}
build test_rtl1 src/core/rtl1_core.c tests/sha256_ref.c
build test_ws src/core/ws_proto.c src/core/json.c
build test_power src/core/power_core.c
build test_rfc2217 src/core/rfc2217_proto.c
build test_console src/core/console_core.c
build test_svs src/core/svs_proto.c src/core/json.c
build test_ota_fetch src/core/ota_fetch_proto.c
build test_buttons src/core/buttons.c
build test_rt4k_info src/core/rt4k_info_core.c
build test_json src/core/json.c
# littlefs as the boards build it (its own warnings aside), under cfgfs over a flash of RAM
LFS=(-DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -I third_party/littlefs)
"$CC" -std=c11 -O1 -g "${LFS[@]}" -c third_party/littlefs/lfs.c -o build-tests/lfs.o &
lfs=$!
"$CC" -std=c11 -O1 -g "${LFS[@]}" -c third_party/littlefs/lfs_util.c -o build-tests/lfs_util.o
wait $lfs
LFS+=(build-tests/lfs.o build-tests/lfs_util.o src/core/cfgfs.c tests/cfgfs_ram.c)
build test_cfgfs "${LFS[@]}"
build test_gameid "${LFS[@]}" src/core/json.c src/core/gameid_core.c src/core/gameid.c

# Each test runs once it's built (a failed build's log says why).
for i in "${!tests[@]}"; do
    if wait "${pids[$i]}"; then
        "./build-tests/${tests[$i]}" > "build-tests/${tests[$i]}.log" 2>&1 &
        pids[$i]=$!
    else
        pids[$i]=0
    fi
done
for i in "${!tests[@]}"; do
    echo "== ${tests[$i]}"
    if [ "${pids[$i]}" = 0 ] || ! wait "${pids[$i]}"; then status=1; fi
    cat "build-tests/${tests[$i]}.log"
    echo
done
wait $page_pid || status=1
cat build-tests/page.log
exit $status
