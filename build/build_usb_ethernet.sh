#!/bin/bash
# Build the USB Ethernet kernel modules (payload/modules) for the stock Hub kernel
# 2.6.31-g89d565c. Run in WSL/Linux.
#
#   KERNEL_DIR    full linux-2.6.31.14 tree with its .config (default ~/linux-2.6.31.14); it is
#                 copied, never changed
#   STOCK_VMLINUX raw stock kernel image (vmlinux.bin, load address 0x80002000)
#   USBCORE_KO    stock usbcore.ko (squashfs-root/lib/modules/2.6.31-g89d565c/...)
#   OUT_DIR       where the .ko files go (default payload/modules)
#
# The stock kernel was built with modversions: a module loads only when it records the kernel's
# CRC for every symbol it uses. A plain build in a tree without Logitech's Module.symvers
# records no CRC for most of them, and the Hub refuses the module ("no symbol version for
# usb_clear_halt", "Unknown symbol ..."). So the CRCs come from the stock image
# (build/extract_symvers.py) and from the stock usbcore.ko, and every module is checked for a
# CRC per undefined symbol before it is copied.
#
# The copy's .config is set like the stock kernel: no SMP, hotplug or suspend, and a 32-bit
# sector_t (no LBDAF). The CRCs are those of the stock image whatever the copy's layout is, so
# modversions cannot catch a layout difference: keep these settings.
#
# The patches in build/patches/usb_net/ are applied to the copy (AX88772B support for asix).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLCHAIN="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
KERNEL_DIR="${KERNEL_DIR:-$HOME/linux-2.6.31.14}"
STOCK_VMLINUX="${STOCK_VMLINUX:-$REPO_ROOT/scratch/vmlinux.bin}"
USBCORE_KO="${USBCORE_KO:-$REPO_ROOT/background/hub_firmware_extracted/squashfs-root/lib/modules/2.6.31-g89d565c/kernel/drivers/usb/core/usbcore.ko}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/payload/modules}"
WORK="${WORK:-$HOME/kbuild-usbnet}"
# r8152 is not here: it is a backport of a later kernel's driver (not in 2.6.31), and the
# r8152.ko in payload/modules already records a CRC for every symbol
MODULES="usbnet asix cdc_ether cdc_subset dm9601 mcs7830 net1080 pegasus rndis_host rtl8150 smsc95xx zaurus"

for f in "$KERNEL_DIR/.config" "$STOCK_VMLINUX" "$USBCORE_KO" "${TOOLCHAIN}gcc"; do
    [ -e "$f" ] || { echo "Error: $f not found" >&2; exit 1; }
done

MAKE=(make ARCH=mips CROSS_COMPILE="$TOOLCHAIN" KCFLAGS=-Wno-error)

echo "=== Copying $KERNEL_DIR to $WORK ==="
rm -rf "$WORK"
cp -a "$KERNEL_DIR" "$WORK"
cd "$WORK"
# modules of an earlier build in the source tree must not be copied as new ones; the Makefile
# names the shipped drivers whatever the .config enables
find drivers/net/usb -name '*.ko' -delete
echo "obj-m := $(echo $MODULES | sed 's/\([a-z0-9_]*\)/\1.o/g')" > drivers/net/usb/Makefile

for p in "$REPO_ROOT"/build/patches/usb_net/*.patch; do
    [ -f "$p" ] || continue
    echo "=== Applying ${p##*/} ==="
    patch -p1 --forward < "$p"
done

echo "=== Configuring like the stock kernel (UP, no hotplug, no suspend, 32-bit sector_t) ==="
sed -i -e 's/^CONFIG_MIPS_MT_SMP=y/# CONFIG_MIPS_MT_SMP is not set\nCONFIG_MIPS_MT_DISABLED=y/' \
       -e 's/^CONFIG_HOTPLUG=y/# CONFIG_HOTPLUG is not set/' \
       -e 's/^CONFIG_LBDAF=y/# CONFIG_LBDAF is not set/' .config
{ yes '' || :; } | "${MAKE[@]}" oldconfig >/dev/null 2>&1
sed -i 's/^CONFIG_SUSPEND=y/# CONFIG_SUSPEND is not set/' .config
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
"${MAKE[@]}" M=drivers/net/usb KBUILD_EXTRA_SYMBOLS="$WORK/usbcore.symvers" modules > usbnet.log 2>&1 || {
    cat usbnet.log >&2
    exit 1
}
if grep 'WARNING: ".*" \[drivers/net/usb/.*\.ko\] undefined' usbnet.log; then
    echo "Error: the modules use symbols the stock kernel does not export" >&2
    exit 1
fi

# every undefined symbol needs a CRC in __versions (module_layout comes on top)
for m in $MODULES; do
    ko="drivers/net/usb/$m.ko"
    [ -f "$ko" ] || { echo "Error: $ko was not built" >&2; exit 1; }
    missing=$(comm -23 <("${TOOLCHAIN}nm" "$ko" | awk '$1 == "U" {print $2}' | sort -u) \
        <(python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import extract_symvers, pathlib; print("\n".join(sorted(extract_symvers.module_versions(pathlib.Path(sys.argv[2])))))' \
            "$REPO_ROOT/build" "$ko" | sort -u))
    if [ -n "$missing" ]; then
        echo "Error: $m.ko has no CRC for: $missing" >&2
        exit 1
    fi
done

mkdir -p "$OUT_DIR"
for m in $MODULES; do
    cp "drivers/net/usb/$m.ko" "$OUT_DIR/"
    "${TOOLCHAIN}strip" --strip-debug "$OUT_DIR/$m.ko"
done
ls -lh "$OUT_DIR"/*.ko
