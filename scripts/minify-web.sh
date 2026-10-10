#!/usr/bin/env bash
# Minifies the page in place, for the images CI builds: its JavaScript (src/web/*.js: comments, whitespace
# and local names go; top-level names stay, the page's scripts share them) and index.html (comments go,
# whitespace collapses to a space where there was some, its CSS minified; ids, attributes and <pre> stay).
# The repo keeps them readable, and a local build embeds them as they are. Needs node (npx fetches esbuild
# and html-minifier-terser).
#   scripts/minify-web.sh
set -euo pipefail
cd "$(dirname "$0")/.."

ESBUILD_VERSION="${ESBUILD_VERSION:-0.28.2}"            # the CI sets them too (.github/workflows/build.yml):
HTML_MINIFIER_VERSION="${HTML_MINIFIER_VERSION:-7.2.0}" # update both

before=$(cat src/web/*.js | wc -c)
npx --yes "esbuild@$ESBUILD_VERSION" src/web/*.js --minify --charset=utf8 --log-level=warning \
    --outdir=src/web --allow-overwrite
after=$(cat src/web/*.js | wc -c)
echo "src/web/*.js minified: $before -> $after bytes"

before=$(wc -c < src/web/index.html)
npx --yes "html-minifier-terser@$HTML_MINIFIER_VERSION" --collapse-whitespace --conservative-collapse \
    --remove-comments --minify-css true -o src/web/index.min.html src/web/index.html
mv src/web/index.min.html src/web/index.html
after=$(wc -c < src/web/index.html)
echo "src/web/index.html minified: $before -> $after bytes"
