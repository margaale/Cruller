#!/usr/bin/env bash
# CI: ninja and ccache from their official releases, checked against their SHA-256, into ~/.local/bin (put on
# the job's PATH). Quicker than apt, whose dpkg database alone took 6-19 s on the runners.
#   scripts/ci-tools.sh
set -euo pipefail

NINJA_VERSION=1.13.2
NINJA_SHA256=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6   # ninja-linux.zip
CCACHE_VERSION=4.14.1
CCACHE_SHA256=ad63d19f5d09ea13f749651653a561c98a59994e673504a4266617b8754218f7  # ccache-...-linux-x86_64-glibc.tar.xz

bin="$HOME/.local/bin"
tmp=$(mktemp -d)
mkdir -p "$bin"

curl -fsSL -o "$tmp/ninja.zip" "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-linux.zip"
echo "$NINJA_SHA256  $tmp/ninja.zip" | sha256sum -c --quiet
unzip -q -o "$tmp/ninja.zip" -d "$bin"

c="ccache-$CCACHE_VERSION-linux-x86_64-glibc"
curl -fsSL -o "$tmp/ccache.tar.xz" "https://github.com/ccache/ccache/releases/download/v$CCACHE_VERSION/$c.tar.xz"
echo "$CCACHE_SHA256  $tmp/ccache.tar.xz" | sha256sum -c --quiet
tar -xJf "$tmp/ccache.tar.xz" -C "$tmp"
cp "$tmp/$c/ccache" "$bin/"

rm -rf "$tmp"
[ -n "${GITHUB_PATH:-}" ] && echo "$bin" >> "$GITHUB_PATH"
echo "ninja $("$bin/ninja" --version), $("$bin/ccache" --version | head -1)"
