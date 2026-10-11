#!/usr/bin/env bash
# CI: ninja and ccache from their official releases, checked against their SHA-256, into ~/.local/bin (put on
# the job's PATH): quicker than apt, whose dpkg database alone took 6-19 s on the runners. And ccache set up
# for the job's compiles: through CMake's launchers (they reach ESP-IDF's bootloader build too), its
# directory .ccache (actions/cache restores it before this), its statistics zeroed.
#   scripts/ci-tools.sh        installs them, sets ccache up
#   scripts/ci-tools.sh done   after the build: drops what it didn't use (a hit refreshes a file's time), so
#                              the cache saved holds this build's files alone; shows the statistics
set -euo pipefail

if [ "${1:-}" = done ]; then
    ccache --evict-older-than "$(($(date +%s) - CCACHE_SINCE + 60))s" > /dev/null
    ccache -s
    exit 0
fi

NINJA_VERSION=1.13.2
NINJA_SHA256=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6   # ninja-linux.zip
CCACHE_VERSION=4.14.1
CCACHE_SHA256=ad63d19f5d09ea13f749651653a561c98a59994e673504a4266617b8754218f7  # ccache-...-linux-x86_64-glibc.tar.xz

bin="$HOME/.local/bin"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$bin"

# fetch <url> <sha256> <file>: downloaded and checked.
fetch() {
    curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --quiet
}

fetch "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-linux.zip" "$NINJA_SHA256" "$tmp/ninja.zip"
unzip -q -o "$tmp/ninja.zip" -d "$bin"
c="ccache-$CCACHE_VERSION-linux-x86_64-glibc"
fetch "https://github.com/ccache/ccache/releases/download/v$CCACHE_VERSION/$c.tar.xz" "$CCACHE_SHA256" "$tmp/ccache.tar.xz"
tar -xJf "$tmp/ccache.tar.xz" -C "$tmp"
cp "$tmp/$c/ccache" "$bin/"

# The next steps' environment. CCACHE_COMPILERCHECK content: ccache checks the compiler, and -specs files,
# by their date by default; the compilers are downloaded again each run, and ESP-IDF writes its
# picolibc.specs again in each build, so by date no file was ever found again.
if [ -n "${GITHUB_ENV:-}" ]; then
    echo "$bin" >> "$GITHUB_PATH"
    cat >> "$GITHUB_ENV" << EOF
CMAKE_C_COMPILER_LAUNCHER=ccache
CMAKE_CXX_COMPILER_LAUNCHER=ccache
CMAKE_ASM_COMPILER_LAUNCHER=ccache
CCACHE_DIR=$GITHUB_WORKSPACE/.ccache
CCACHE_COMPILERCHECK=content
CCACHE_MAXSIZE=150M
CCACHE_SINCE=$(date +%s)
EOF
    CCACHE_DIR="$GITHUB_WORKSPACE/.ccache" "$bin/ccache" -z > /dev/null
fi
echo "ninja $("$bin/ninja" --version), $("$bin/ccache" --version | head -1)"
