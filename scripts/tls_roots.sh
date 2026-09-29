#!/usr/bin/env bash
# Writes src/platform/rp2/tls_roots.c: the root certificates the Pico 2 W trusts for HTTPS (its own
# firmware downloads from GitHub, src/platform/rp2/tls.c), as DER, taken from a CA bundle in PEM
# (Mozilla's list, as curl and Git ship it). The ESP32-S3 uses ESP-IDF's bundle instead. Needs openssl.
#   scripts/tls_roots.sh [bundle.pem]
set -euo pipefail
cd "$(dirname "$0")/.."

bundle="${1:-/usr/ssl/certs/ca-bundle.crt}"
out=src/platform/rp2/tls_roots.c

# github.com: Sectigo's Root E46, which GitHub sends cross-signed by USERTrust ECC. Its release assets
# (release-assets.githubusercontent.com): Let's Encrypt, under ISRG Root X1. The rest in case either
# moves: their RSA and ECDSA siblings, and DigiCert, GitHub's CA until 2024.
roots=(
    "ISRG Root X1"
    "ISRG Root X2"
    "USERTrust ECC Certification Authority"
    "Sectigo Public Server Authentication Root E46"
    "USERTrust RSA Certification Authority"
    "Sectigo Public Server Authentication Root R46"
    "DigiCert Global Root G2"
)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# One file per certificate, then the wanted ones by common name (the first of each).
awk -v dir="$tmp" '/-----BEGIN CERTIFICATE-----/ { f = sprintf("%s/cert%04d.pem", dir, ++n) }
    f { print > f } /-----END CERTIFICATE-----/ { close(f); f = "" }' "$bundle"
for pem in "$tmp"/cert*.pem; do
    cn=$(openssl x509 -in "$pem" -noout -subject -nameopt multiline | sed -n 's/^ *commonName *= //p')
    for i in "${!roots[@]}"; do
        if [ "$cn" = "${roots[$i]}" ] && [ ! -e "$tmp/root$i.pem" ]; then cp "$pem" "$tmp/root$i.pem"; fi
    done
done

{
    echo "// The root certificates the Pico 2 W trusts for HTTPS (tls.c), as DER. Written by"
    echo "// scripts/tls_roots.sh from $(basename "$bundle"): run it again to change them."
    echo
    echo '#include "tls_roots.h"'
    for i in "${!roots[@]}"; do
        pem="$tmp/root$i.pem"
        if [ ! -e "$pem" ]; then echo "${roots[$i]}: not in $bundle" >&2; exit 1; fi
        until=$(date -d "$(openssl x509 -in "$pem" -noout -enddate | cut -d= -f2)" +%Y-%m-%d)
        fp=$(openssl x509 -in "$pem" -noout -fingerprint -sha256 | cut -d= -f2 | tr -d : | tr A-F a-f)
        echo
        echo "// ${roots[$i]}, until $until, SHA-256 $fp"
        echo "static const uint8_t root$i[] = {"
        openssl x509 -in "$pem" -outform DER | od -An -v -tx1 | sed -E 's/ ([0-9a-f]{2})/ 0x\1,/g; s/^ /    /'
        echo "};"
    done
    echo
    echo "const tls_root_t tls_roots[] = {"
    for i in "${!roots[@]}"; do echo "    {\"${roots[$i]}\", root$i, sizeof(root$i)},"; done
    echo "};"
    echo "const size_t tls_roots_count = sizeof(tls_roots) / sizeof(tls_roots[0]);"
} > "$out"
echo "wrote $out (${#roots[@]} roots)"
