#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TOOLCHAIN="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
KERNEL_DIR="${KERNEL_DIR:-/home/yr/linux-2.6.31.14}"
OUT_DIR="$REPO_ROOT/payload/modules"

if [ ! -d "$KERNEL_DIR" ]; then
    echo "Error: Kernel tree not found at $KERNEL_DIR" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

echo "=== Building USB Ethernet Kernel Modules ==="
echo "Kernel: $KERNEL_DIR"
echo "Toolchain: $TOOLCHAIN"
echo "Output: $OUT_DIR"

make -C "$KERNEL_DIR" \
    M="$KERNEL_DIR/drivers/net/usb" \
    modules \
    ARCH=mips \
    CROSS_COMPILE="$TOOLCHAIN"

echo "=== Copying built modules to $OUT_DIR ==="
cp -v "$KERNEL_DIR/drivers/net/usb/"*.ko "$OUT_DIR/"

echo "=== Staging complete ==="
ls -lh "$OUT_DIR"/*.ko
