#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TMP_DIR="$SCRIPT_DIR/tmp"
BB_VER="1.36.1"
BB_TAR="busybox-${BB_VER}.tar.bz2"
BB_URL="https://busybox.net/downloads/${BB_TAR}"

mkdir -p "$TMP_DIR" "$REPO_ROOT/payload/bin" "$REPO_ROOT/elite/bin"

TARGET="${1:-all}"

# Ensure archive downloaded
if [ ! -f "$TMP_DIR/$BB_TAR" ]; then
    echo "[-] Downloading BusyBox ${BB_VER}..."
    curl -sSL -o "$TMP_DIR/$BB_TAR" "$BB_URL"
fi

build_mips() {
    echo "=== Building BusyBox ${BB_VER} for Hub (MIPS big-endian) ==="
    MIPS_BUILD="$TMP_DIR/busybox-${BB_VER}-mips"
    rm -rf "$MIPS_BUILD"
    mkdir -p "$MIPS_BUILD"
    tar -xjf "$TMP_DIR/$BB_TAR" -C "$MIPS_BUILD" --strip-components=1

    cd "$MIPS_BUILD"
    MIPS_BIN="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin"
    export PATH="$MIPS_BIN:$PATH"

    make defconfig
    # Enable static compilation and disable tc (requires linux kernel headers) and seedrng (requires getrandom syscall)
    sed -i 's/.*CONFIG_STATIC.*/CONFIG_STATIC=y/' .config
    sed -i 's/.*CONFIG_STATIC_LIBGCC.*/CONFIG_STATIC_LIBGCC=y/' .config
    sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config
    sed -i 's/CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' .config
    sed -i 's/CONFIG_SEEDRNG=y/# CONFIG_SEEDRNG is not set/' .config
    scripts/kconfig/conf -D .config Config.in

    make -j"$(nproc)" CROSS_COMPILE="mips-buildroot-linux-uclibc-" busybox
    mips-buildroot-linux-uclibc-strip busybox

    cp busybox "$REPO_ROOT/payload/bin/busybox"
    echo "[+] MIPS BusyBox built: $REPO_ROOT/payload/bin/busybox ($(ls -lh "$REPO_ROOT/payload/bin/busybox" | awk '{print $5}'))"
}

build_arm() {
    echo "=== Building BusyBox ${BB_VER} for Elite Remote (ARM little-endian uClibc) ==="
    ARM_BUILD="$TMP_DIR/busybox-${BB_VER}-arm"
    rm -rf "$ARM_BUILD"
    mkdir -p "$ARM_BUILD"
    tar -xjf "$TMP_DIR/$BB_TAR" -C "$ARM_BUILD" --strip-components=1

    cd "$ARM_BUILD"
    ARM_BIN="$REPO_ROOT/build/toolchains/armv5-eabi--uclibc--stable/bin"
    export PATH="$ARM_BIN:$PATH"

    make defconfig
    # Enable static compilation and disable tc and seedrng
    sed -i 's/.*CONFIG_STATIC.*/CONFIG_STATIC=y/' .config
    sed -i 's/.*CONFIG_STATIC_LIBGCC.*/CONFIG_STATIC_LIBGCC=y/' .config
    sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config
    sed -i 's/CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' .config
    sed -i 's/CONFIG_SEEDRNG=y/# CONFIG_SEEDRNG is not set/' .config
    scripts/kconfig/conf -D .config Config.in
    set -euo pipefail

    make -j"$(nproc)" CROSS_COMPILE="arm-buildroot-linux-uclibcgnueabi-" busybox
    arm-buildroot-linux-uclibcgnueabi-strip busybox

    cp busybox "$REPO_ROOT/elite/bin/busybox"
    echo "[+] ARM BusyBox built: $REPO_ROOT/elite/bin/busybox ($(ls -lh "$REPO_ROOT/elite/bin/busybox" | awk '{print $5}'))"
}

case "$TARGET" in
    mips|hub)
        build_mips
        ;;
    arm|elite)
        build_arm
        ;;
    all)
        build_mips
        build_arm
        ;;
    *)
        echo "Usage: $0 [mips|arm|all]"
        exit 1
        ;;
esac

echo "[+] BusyBox compilation complete!"
