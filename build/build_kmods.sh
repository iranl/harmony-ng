#!/bin/bash
# Build the USB storage kernel modules (scsi_mod, sd_mod, usb-storage) for the stock Hub kernel
# 2.6.31-g89d565c. Run in WSL/Linux.
#
#   KERNEL_TARBALL kernel.org linux-2.6.31.14.tar.xz (default ~/linux-2.6.31.14.tar.xz)
#   STOCK_VMLINUX  raw stock kernel image (vmlinux.bin, load address 0x80002000)
#   USBCORE_KO     stock usbcore.ko (squashfs-root/lib/modules/2.6.31-g89d565c/...)
#   OUT_DIR        where the .ko files go (default payload/modules)
# The three inputs are checked against the SHA-256 values below.
#
# The tree and .config are those of the USB Ethernet modules (build_usb_ethernet.sh): the tarball
# with build/patches/usb_net/kernel-tree.patch and build/patches/usb_net/kernel.config (no SMP,
# hotplug or suspend, a 32-bit sector_t, HZ 250), plus CONFIG_USB_STORAGE=m. The CRCs of the
# kernel's exports come from the stock image (build/extract_symvers.py, checked against the CRCs
# of payload/modules) and those of usbcore from the stock usbcore.ko.
#
# The CRCs say nothing about the layout behind them: build/patches/usb_storage/stock_layout.c is
# built with usb-storage and fails to compile unless struct request, HZ and the kmalloc table
# match the stock kernel. Load and test the modules on a Hub before they go to payload/modules
# (from there publish_ota.py offers them to every Hub).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCHES="$REPO_ROOT/build/patches/usb_net"
TOOLCHAIN="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
KERNEL_TARBALL="${KERNEL_TARBALL:-$HOME/linux-2.6.31.14.tar.xz}"
STOCK_VMLINUX="${STOCK_VMLINUX:-$REPO_ROOT/scratch/vmlinux.bin}"
USBCORE_KO="${USBCORE_KO:-$REPO_ROOT/background/hub_firmware_extracted/squashfs-root/lib/modules/2.6.31-g89d565c/kernel/drivers/usb/core/usbcore.ko}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/payload/modules}"
WORK="${WORK:-$HOME/kbuild-storage}"

for f in "$KERNEL_TARBALL" "$STOCK_VMLINUX" "$USBCORE_KO" "${TOOLCHAIN}gcc"; do
    [ -e "$f" ] || { echo "Error: $f not found" >&2; exit 1; }
done
sha256sum -c --quiet - <<EOF || { echo "Error: an input is not the expected file" >&2; exit 1; }
6546b025e80aeef3eb90d42fad2d94500d23fb435c57664c444ccd358575c588  $KERNEL_TARBALL
0baeda5b82a7ac9d91b10af0fd6fa399e6da4f71c49cd011ee59880f13f51bd2  $STOCK_VMLINUX
95e4fc75526f64a9e9364893f5665c0629bb8f295cb6f3ab9ed448e3a2063ff7  $USBCORE_KO
EOF

MAKE=(make ARCH=mips CROSS_COMPILE="$TOOLCHAIN" KCFLAGS=-Wno-error)

echo "=== Unpacking $KERNEL_TARBALL to $WORK ==="
rm -rf "$WORK"
mkdir -p "$WORK"
tar -xJf "$KERNEL_TARBALL" -C "$WORK" --strip-components=1
cd "$WORK"
patch -p1 --forward < "$PATCHES/kernel-tree.patch"
tr -d '\r' < "$PATCHES/kernel.config" > .config  # a Windows checkout has CRLF
cp "$REPO_ROOT/build/patches/usb_storage/stock_layout.c" drivers/usb/storage/
echo 'obj-m += stock_layout.o' >> drivers/usb/storage/Makefile

echo "=== Configuring like the stock kernel + USB storage ==="
sed -i 's/^# CONFIG_USB_STORAGE is not set$/CONFIG_USB_STORAGE=m/' .config
{ yes '' || :; } | "${MAKE[@]}" oldconfig > oldconfig.log 2>&1 || { cat oldconfig.log >&2; exit 1; }
grep -q '^CONFIG_USB_STORAGE=m' .config || { echo "Error: CONFIG_USB_STORAGE is not a module" >&2; exit 1; }
for opt in SMP HOTPLUG SUSPEND FREEZER LBDAF; do
    if grep -q "^CONFIG_$opt=y" .config; then
        echo "Error: CONFIG_$opt is still on; the modules would not match the stock kernel" >&2
        exit 1
    fi
done
"${MAKE[@]}" modules_prepare >/dev/null

echo "=== Stock CRCs ==="
python3 "$REPO_ROOT/build/extract_symvers.py" "$STOCK_VMLINUX" Module.symvers --check "$REPO_ROOT/payload/modules"
"${TOOLCHAIN}nm" "$USBCORE_KO" | awk '$3 ~ /^__crc_/ {sub("__crc_","",$3); printf "0x%s\t%s\tdrivers/usb/core/usbcore\tEXPORT_SYMBOL\n", $1, $3}' > usbcore.symvers

echo "=== Building ==="
"${MAKE[@]}" M=drivers/scsi modules > scsi.log 2>&1 || { cat scsi.log >&2; exit 1; }
"${MAKE[@]}" M=drivers/usb/storage KBUILD_EXTRA_SYMBOLS="$WORK/usbcore.symvers $WORK/drivers/scsi/Module.symvers" modules > storage.log 2>&1 || {
    cat storage.log >&2
    exit 1
}
if grep -h 'WARNING: ".*" \[drivers/\(scsi/scsi_mod\|scsi/sd_mod\|usb/storage/usb-storage\)\.ko\] undefined' scsi.log storage.log; then
    echo "Error: the modules use symbols the stock kernel does not export" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
cp -v drivers/scsi/scsi_mod.ko drivers/scsi/sd_mod.ko drivers/usb/storage/usb-storage.ko "$OUT_DIR/"
"${TOOLCHAIN}strip" --strip-debug "$OUT_DIR"/scsi_mod.ko "$OUT_DIR"/sd_mod.ko "$OUT_DIR"/usb-storage.ko
ls -lh "$OUT_DIR"/scsi_mod.ko "$OUT_DIR"/sd_mod.ko "$OUT_DIR"/usb-storage.ko
