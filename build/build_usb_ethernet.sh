#!/bin/bash
# Build the USB Ethernet kernel modules (payload/modules) for the stock Hub kernel
# 2.6.31-g89d565c. Run in WSL/Linux.
#
#   KERNEL_TARBALL kernel.org linux-2.6.31.14.tar.xz (default ~/linux-2.6.31.14.tar.xz,
#                  https://cdn.kernel.org/pub/linux/kernel/v2.6/linux-2.6.31.14.tar.xz)
#   STOCK_VMLINUX  raw stock kernel image (vmlinux.bin, load address 0x80002000): the lzma
#                  payload of the mtd1 uImage (firmware 4.15.600)
#   USBCORE_KO     stock usbcore.ko (squashfs-root/lib/modules/2.6.31-g89d565c/...)
# The three inputs are checked against the SHA-256 values below.
#   OUT_DIR        where the .ko files go (default payload/modules)
#
# The tree is the tarball plus build/patches/usb_net/kernel-tree.patch (stock EXTRAVERSION and
# vermagic, compiler-gcc5.h for the Buildroot gcc 5.4 toolchain, no ZONE_DMA select, a kmalloc
# shim in asix.c: its kmalloc calls go out of line to __kmalloc and do not index the kernel's
# malloc_sizes[]; kept from the tree the shipped asix.ko was first built in), then the other
# patches in that folder (AX88772B support for asix). The .config is build/patches/usb_net/kernel.config. kernel.org has no AR933x platform, so that
# file is a Malta board config, not the stock one: what must match are the values that reach
# module code (struct layouts, HZ, the kmalloc table), and stock_layout.c checks those.
#
# The stock kernel was built with modversions: a module loads only when it records the kernel's
# CRC for every symbol it uses. A plain build in a tree without Logitech's Module.symvers
# records no CRC for most of them, and the Hub refuses the module ("no symbol version for
# usb_clear_halt", "Unknown symbol ..."). So the CRCs come from the stock image
# (build/extract_symvers.py) and from the stock usbcore.ko, and every module is checked for a
# CRC per undefined symbol before it is copied.
#
# The .config is set like the stock kernel: no SMP, hotplug or suspend, a 32-bit sector_t (no
# LBDAF), and no IPv6 or XFRM (both add members to struct sk_buff). The CRCs are those of the
# stock image whatever the copy's layout is, so modversions cannot catch a layout difference:
# build/patches/usb_net/stock_layout.c is built with the drivers and fails to compile unless
# sk_buff, net_device, urb, usb_device and usb_interface have the stock offsets.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCHES="$REPO_ROOT/build/patches/usb_net"
TOOLCHAIN="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
KERNEL_TARBALL="${KERNEL_TARBALL:-$HOME/linux-2.6.31.14.tar.xz}"
STOCK_VMLINUX="${STOCK_VMLINUX:-$REPO_ROOT/scratch/vmlinux.bin}"
USBCORE_KO="${USBCORE_KO:-$REPO_ROOT/background/hub_firmware_extracted/squashfs-root/lib/modules/2.6.31-g89d565c/kernel/drivers/usb/core/usbcore.ko}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/payload/modules}"
WORK="${WORK:-$HOME/kbuild-usbnet}"
# r8152 is not here: it is a backport of a later kernel's driver (not in 2.6.31), and the
# r8152.ko in payload/modules already records a CRC for every symbol
MODULES="usbnet asix cdc_ether cdc_subset dm9601 mcs7830 net1080 pegasus rndis_host rtl8150 smsc95xx zaurus"

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
# the Makefile names the shipped drivers whatever the .config enables
echo "obj-m := $(echo $MODULES | sed 's/\([a-z0-9_]*\)/\1.o/g') stock_layout.o" > drivers/net/usb/Makefile
cp "$PATCHES/stock_layout.c" drivers/net/usb/
cp "$PATCHES/kernel.config" .config

for p in "$PATCHES/kernel-tree.patch" $(ls "$PATCHES"/*.patch | grep -v '/kernel-tree\.patch$'); do
    echo "=== Applying ${p##*/} ==="
    patch -p1 --forward < "$p"
done

echo "=== Configuring like the stock kernel (UP, no hotplug, no suspend, 32-bit sector_t, no IPv6/XFRM) ==="
# an option set to "is not set" is answered, so oldconfig does not turn it back on; XFRM has no
# prompt and goes once nothing selects it
OFF='MIPS_MT_SMP|HOTPLUG|LBDAF|SUSPEND|IPV6|IPV6_[A-Z0-9_]*|INET6_[A-Z0-9_]*|XFRM_[A-Z0-9_]*|INET_XFRM_[A-Z0-9_]*|INET_AH|INET_ESP|INET_IPCOMP|NET_KEY|NET_KEY_MIGRATE'
sed -i -r "s/^CONFIG_($OFF)=[ym]$/# CONFIG_\1 is not set/" .config
grep -q '^CONFIG_MIPS_MT_DISABLED=y' .config || echo 'CONFIG_MIPS_MT_DISABLED=y' >> .config
oldconfig() {
    { yes '' || :; } | "${MAKE[@]}" oldconfig > oldconfig.log 2>&1 || { cat oldconfig.log >&2; exit 1; }
}
oldconfig
sed -i -r "s/^CONFIG_($OFF)=[ym]$/# CONFIG_\1 is not set/" .config
oldconfig
for opt in SMP HOTPLUG SUSPEND FREEZER LBDAF IPV6 XFRM; do
    if grep -q "^CONFIG_$opt=[ym]" .config; then
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
