#!/bin/bash
# Build the USB storage kernel modules (scsi_mod, sd_mod, usb-storage) for the stock Hub kernel
# 2.6.31-g89d565c. Run in WSL/Linux.
#
#   KERNEL_DIR   full linux-2.6.31.14 tree with the .config used for payload/modules
#                (default ~/linux-2.6.31.14); it is copied, never changed
#   STOCK_VMLINUX raw stock kernel image (vmlinux.bin, load address 0x80002000)
#   USBCORE_KO   stock usbcore.ko (squashfs-root/lib/modules/2.6.31-g89d565c/...)
#   OUT_DIR      where the .ko files go (default payload/modules)
#
# The stock kernel was built with modversions, without SMP, hotplug (no kobject_uevent_env
# export) and suspend (no refrigerator export): the copy's .config is set the same way. The CRCs
# of the kernel's exports come from the stock image (build/extract_symvers.py, checked against
# the CRCs of payload/modules) and those of usbcore from the stock usbcore.ko.
#
# Only the CRCs are checked this way, not the layout of the structures behind them: load and
# test the modules on a Hub with a serial console before they go to payload/modules (from there
# publish_ota.py offers them to every Hub).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLCHAIN="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
KERNEL_DIR="${KERNEL_DIR:-$HOME/linux-2.6.31.14}"
STOCK_VMLINUX="${STOCK_VMLINUX:-$REPO_ROOT/scratch/vmlinux.bin}"
USBCORE_KO="${USBCORE_KO:-$REPO_ROOT/background/hub_firmware_extracted/squashfs-root/lib/modules/2.6.31-g89d565c/kernel/drivers/usb/core/usbcore.ko}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/payload/modules}"
WORK="${WORK:-$HOME/kbuild-storage}"

for f in "$KERNEL_DIR/.config" "$STOCK_VMLINUX" "$USBCORE_KO" "${TOOLCHAIN}gcc"; do
    [ -e "$f" ] || { echo "Error: $f not found" >&2; exit 1; }
done

MAKE=(make ARCH=mips CROSS_COMPILE="$TOOLCHAIN" KCFLAGS=-Wno-error)

echo "=== Copying $KERNEL_DIR to $WORK ==="
rm -rf "$WORK"
cp -a "$KERNEL_DIR" "$WORK"
cd "$WORK"

echo "=== Configuring like the stock kernel (UP, no hotplug, no suspend) + USB storage ==="
sed -i -e 's/^CONFIG_MIPS_MT_SMP=y/# CONFIG_MIPS_MT_SMP is not set\nCONFIG_MIPS_MT_DISABLED=y/' \
       -e 's/^CONFIG_HOTPLUG=y/# CONFIG_HOTPLUG is not set/' .config
echo 'CONFIG_USB_STORAGE=m' >> .config
{ yes '' || :; } | "${MAKE[@]}" oldconfig >/dev/null 2>&1
sed -i 's/^CONFIG_SUSPEND=y/# CONFIG_SUSPEND is not set/' .config
# The stock kernel has a 32-bit sector_t: its blk_rq_init clears a 196-byte struct request and
# sets __sector with one word. With LBDAF on, struct request is 200 bytes and scsi_mod hits a
# BUG in scsi_setup_blk_pc_cmnd at the first scan. The CRCs come from the stock image, so
# modversions cannot catch such a layout difference.
sed -i 's/^CONFIG_LBDAF=y/# CONFIG_LBDAF is not set/' .config
{ yes '' || :; } | "${MAKE[@]}" oldconfig >/dev/null 2>&1
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
"${MAKE[@]}" M=drivers/scsi modules > scsi.log 2>&1
"${MAKE[@]}" M=drivers/usb/storage KBUILD_EXTRA_SYMBOLS="$WORK/usbcore.symvers $WORK/drivers/scsi/Module.symvers" modules > storage.log 2>&1
if grep -h 'WARNING: ".*" \[drivers/\(scsi/scsi_mod\|scsi/sd_mod\|usb/storage/usb-storage\)\.ko\] undefined' scsi.log storage.log; then
    echo "Error: the modules use symbols the stock kernel does not export" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
cp -v drivers/scsi/scsi_mod.ko drivers/scsi/sd_mod.ko drivers/usb/storage/usb-storage.ko "$OUT_DIR/"
"${TOOLCHAIN}strip" --strip-debug "$OUT_DIR"/scsi_mod.ko "$OUT_DIR"/sd_mod.ko "$OUT_DIR"/usb-storage.ko
ls -lh "$OUT_DIR"/scsi_mod.ko "$OUT_DIR"/sd_mod.ko "$OUT_DIR"/usb-storage.ko
