#!/usr/bin/env bash
# Minifies the page's JavaScript (src/web/*.js) in place, for the images CI builds: comments, whitespace
# and local names go; top-level names stay (the page's scripts share them). The repo keeps them readable,
# and a local build embeds them as they are. Needs node (npx fetches esbuild).
#   scripts/minify-web.sh
set -euo pipefail
cd "$(dirname "$0")/.."

ESBUILD_VERSION="${ESBUILD_VERSION:-0.28.2}" # the CI sets it too (.github/workflows/build.yml): update both

before=$(cat src/web/*.js | wc -c)
npx --yes "esbuild@$ESBUILD_VERSION" src/web/*.js --minify --charset=utf8 --log-level=warning \
    --outdir=src/web --allow-overwrite
after=$(cat src/web/*.js | wc -c)
echo "src/web/*.js minified: $before -> $after bytes"
