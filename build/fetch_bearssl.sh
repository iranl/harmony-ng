#!/bin/sh
# BearSSL sources at the pinned commit in build/tmp/bearssl (or BEARSSL_DIR).
# The commit is part of the ABI: BearSSL contexts are structs the caller allocates, so plugins
# linked against payload/lib/libbearssl.so.0 only work with the same commit. A new commit needs
# a new SONAME (libbearssl.so.1) in build/compile_all.sh and a plugin rebuild.
set -eu

BEARSSL_COMMIT="7bea48e5e850ab4cafbe68d3765cdaba13a86d6f"
BEARSSL_URL="${BEARSSL_URL:-https://bearssl.org/git/BearSSL}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BEARSSL_DIR="${BEARSSL_DIR:-$SCRIPT_DIR/tmp/bearssl}"

if [ -d "$BEARSSL_DIR/.git" ] && [ "$(git -C "$BEARSSL_DIR" rev-parse HEAD 2>/dev/null)" = "$BEARSSL_COMMIT" ]; then
    exit 0
fi
if [ -d "$BEARSSL_DIR" ]; then
    if [ -d "$BEARSSL_DIR/.git" ] && git -C "$BEARSSL_DIR" cat-file -e "$BEARSSL_COMMIT^{commit}" 2>/dev/null; then
        git -C "$BEARSSL_DIR" checkout -q "$BEARSSL_COMMIT"
        rm -f "$BEARSSL_DIR"/libbearssl*.a "$BEARSSL_DIR"/libbearssl.flags
        exit 0
    fi
    echo "[!] $BEARSSL_DIR exists but is not BearSSL at $BEARSSL_COMMIT; remove it and run again" >&2
    exit 1
fi
echo "[-] Fetching BearSSL $BEARSSL_COMMIT..."
mkdir -p "$(dirname "$BEARSSL_DIR")"
git clone -q "$BEARSSL_URL" "$BEARSSL_DIR"
git -C "$BEARSSL_DIR" checkout -q "$BEARSSL_COMMIT"
