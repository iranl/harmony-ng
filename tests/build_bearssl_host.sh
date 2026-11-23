#!/bin/sh
# Host (x86) libbearssl.a for the tests: build/tmp/test_host/bearssl/libbearssl.a.
# Prints the library path. Rebuilt when the BearSSL commit changes.
set -eu
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BEARSSL_DIR="${BEARSSL_DIR:-$REPO_ROOT/build/tmp/bearssl}"
OUT="$REPO_ROOT/build/tmp/test_host/bearssl"
BEARSSL_DIR="$BEARSSL_DIR" sh "$REPO_ROOT/build/fetch_bearssl.sh" >&2
stamp=$(git -C "$BEARSSL_DIR" rev-parse HEAD)
if [ ! -f "$OUT/libbearssl.a" ] || [ "$(cat "$OUT/stamp" 2>/dev/null)" != "$stamp" ]; then
    echo "[-] Building host libbearssl.a..." >&2
    rm -rf "$OUT"
    mkdir -p "$OUT/obj"
    C_FILES=$(cd "$BEARSSL_DIR" && find src -name '*.c' ! -name 'brssl.c' ! -name 'test_*.c' | sed "s|^|$BEARSSL_DIR/|")
    (cd "$OUT/obj" && gcc -O2 -fPIC -I"$BEARSSL_DIR/inc" -I"$BEARSSL_DIR/src" -c $C_FILES)
    ar rcs "$OUT/libbearssl.a" "$OUT"/obj/*.o
    rm -rf "$OUT/obj"
    echo "$stamp" > "$OUT/stamp"
fi
echo "$OUT/libbearssl.a"
