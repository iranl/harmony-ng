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
    for t in "$REPO_ROOT/build/toolchains"/mips32--uclibc--stable*; do
        if [ -d "$t/bin" ]; then
            export PATH="$t/bin:$PATH"
            break
        fi
    done

    make defconfig
    # Enable static compilation and disable tc (requires linux kernel headers) and seedrng (requires getrandom syscall)
    sed -i 's/.*CONFIG_STATIC.*/CONFIG_STATIC=y/' .config
    sed -i 's/.*CONFIG_STATIC_LIBGCC.*/CONFIG_STATIC_LIBGCC=y/' .config
    sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config
    sed -i 's/CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' .config
    sed -i 's/CONFIG_SEEDRNG=y/# CONFIG_SEEDRNG is not set/' .config

    # Enable built-in TLS & HTTPS for wget
    sed -i 's/.*CONFIG_TLS.*/CONFIG_TLS=y/' .config
    sed -i 's/.*CONFIG_FEATURE_WGET_HTTPS.*/CONFIG_FEATURE_WGET_HTTPS=y/' .config

    # Prune unused bulky applets to keep binary size compact
    for opt in CONFIG_BC CONFIG_DC CONFIG_DPKG CONFIG_DPKG_DEB CONFIG_RPM \
               CONFIG_FDISK CONFIG_HTTPD CONFIG_TELNETD CONFIG_TFTP CONFIG_TFTPD \
               CONFIG_FTPD CONFIG_LPD CONFIG_CHAT CONFIG_CONSPY CONFIG_UBIATTACH \
               CONFIG_UBIDETACH CONFIG_UBIMKVOL CONFIG_UBIRMVOL CONFIG_UBIRSVOL \
               CONFIG_UBIRENAME CONFIG_RX CONFIG_ZCIP; do
        sed -i "s/^${opt}=y/# ${opt} is not set/" .config
    done
    scripts/kconfig/conf -D .config Config.in

    make -j"$(nproc)" CROSS_COMPILE="mips-buildroot-linux-uclibc-" busybox
    mips-buildroot-linux-uclibc-strip busybox

    cp busybox "$REPO_ROOT/payload/bin/busybox"
    echo "[+] MIPS BusyBox built: $REPO_ROOT/payload/bin/busybox ($(ls -lh "$REPO_ROOT/payload/bin/busybox" | awk '{print $5}'))"
}

build_arm() {
    echo "=== Building BusyBox ${BB_VER} for Elite Remote (ARM little-endian) ==="
    ARM_BUILD="$TMP_DIR/busybox-${BB_VER}-arm"
    rm -rf "$ARM_BUILD"
    mkdir -p "$ARM_BUILD"
    tar -xjf "$TMP_DIR/$BB_TAR" -C "$ARM_BUILD" --strip-components=1

    cd "$ARM_BUILD"
    for t in "$REPO_ROOT/build/toolchains"/armv5-eabi--uclibc--stable*; do
        if [ -d "$t/bin" ]; then
            export PATH="$t/bin:$PATH"
            break
        fi
    done

    make defconfig
    # Enable static compilation and disable tc and seedrng
    sed -i 's/.*CONFIG_STATIC.*/CONFIG_STATIC=y/' .config
    sed -i 's/.*CONFIG_STATIC_LIBGCC.*/CONFIG_STATIC_LIBGCC=y/' .config
    sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config
    sed -i 's/CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' .config
    sed -i 's/CONFIG_SEEDRNG=y/# CONFIG_SEEDRNG is not set/' .config

    # The Elite runs Linux 2.6.37. A static glibc BusyBox breaks there (tar -c,
    # find and du fail), so only the uClibc toolchain is accepted.
    CROSS="arm-buildroot-linux-uclibcgnueabi-"
    if ! command -v "${CROSS}gcc" >/dev/null 2>&1; then
        echo "[!] ${CROSS}gcc not found. Extract the Bootlin armv5-eabi--uclibc--stable toolchain into build/toolchains/." >&2
        exit 1
    fi
    scripts/kconfig/conf -D .config Config.in
    make -j"$(nproc)" CROSS_COMPILE="$CROSS" busybox
    "${CROSS}strip" busybox

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
