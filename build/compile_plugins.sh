#!/bin/bash
set -eu

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLUGINS_ROOT=$(cd "$REPO_ROOT/../harmony-ng-plugins" && pwd)
TOOLCHAIN_BIN="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin"

if [ -d "$TOOLCHAIN_BIN" ]; then
    export PATH="$TOOLCHAIN_BIN:$PATH"
fi

CC="mips-buildroot-linux-uclibc-gcc"
AR="mips-buildroot-linux-uclibc-ar"
STRIP="mips-buildroot-linux-uclibc-strip"
BEARSSL_DIR="$REPO_ROOT/build/tmp/bearssl"

echo "=== Building Harmony-NG Plugins for MIPS32 ==="
echo "Toolchain: $CC"
echo "Plugins directory: $PLUGINS_ROOT"

# 1. Compile BearSSL static library if not built
if [ ! -f "$BEARSSL_DIR/libbearssl.a" ]; then
    echo "[*] Compiling BearSSL library for MIPS..."
    cd "$BEARSSL_DIR"
    C_FILES=$(find src -name '*.c' ! -name 'brssl.c' ! -name 'test_*.c')
    $CC -Os -fPIC -Iinc -Isrc -c $C_FILES
    $AR rcs libbearssl.a *.o
    rm -f *.o
    echo "[+] libbearssl.a built: $(ls -lh libbearssl.a | awk '{print $5}')"
fi

# 2. Build roku-ecp
echo "[*] Compiling roku-ecp..."
mkdir -p "$PLUGINS_ROOT/roku-ecp/bin"
$CC -Wall -Os -s "$PLUGINS_ROOT/roku-ecp/roku_ecp.c" -o "$PLUGINS_ROOT/roku-ecp/bin/roku_ecp"
$STRIP "$PLUGINS_ROOT/roku-ecp/bin/roku_ecp"
echo "[+] roku_ecp binary built: $(ls -lh "$PLUGINS_ROOT/roku-ecp/bin/roku_ecp" | awk '{print $5}')"

# 3. Build ps5-wake
echo "[*] Compiling ps5-wake..."
mkdir -p "$PLUGINS_ROOT/ps5-wake/bin"
$CC -Wall -Os -s "$PLUGINS_ROOT/ps5-wake/ps5_wake.c" -o "$PLUGINS_ROOT/ps5-wake/bin/ps5_wake"
$STRIP "$PLUGINS_ROOT/ps5-wake/bin/ps5_wake"
echo "[+] ps5_wake binary built: $(ls -lh "$PLUGINS_ROOT/ps5-wake/bin/ps5_wake" | awk '{print $5}')"

# 4. Build android-tv
echo "[*] Compiling android-tv with BearSSL..."
mkdir -p "$PLUGINS_ROOT/android-tv/bin"
$CC -Wall -Os -s -I"$BEARSSL_DIR/inc" "$PLUGINS_ROOT/android-tv/android_tv.c" "$BEARSSL_DIR/libbearssl.a" -o "$PLUGINS_ROOT/android-tv/bin/android_tv"
$STRIP "$PLUGINS_ROOT/android-tv/bin/android_tv"
echo "[+] android_tv binary built: $(ls -lh "$PLUGINS_ROOT/android-tv/bin/android_tv" | awk '{print $5}')"

# 5. Package plugins into dist/
mkdir -p "$PLUGINS_ROOT/dist"
for p in roku-ecp ps5-wake android-tv; do
    echo "[*] Packaging $p..."
    tar -czf "$PLUGINS_ROOT/dist/${p}-plugin.tar.gz" -C "$PLUGINS_ROOT/${p}" manifest.json run.sh bin/
    echo "[+] Created dist/${p}-plugin.tar.gz ($(ls -lh "$PLUGINS_ROOT/dist/${p}-plugin.tar.gz" | awk '{print $5}'))"
done

echo "=== All plugins compiled and packaged successfully! ==="
