#!/bin/bash
# Build a modern (kernel.org LTS) kernel for the Harmony Hub (AR9330). Run in WSL/Linux.
#
# Output in $OUT_DIR (default build/output/kernel6):
#   harmony-hub-kernel.uImage     kernel + appended DTB, LZMA uImage for a flash partition.
#                                 Reported against the 960 KB (0xF0000) kernel/kernel2 size.
#   harmony-hub-initramfs.uImage  same kernel with a BusyBox shell in RAM, for a serial test
#                                 boot from U-Boot (loady/loadb to 0x81000000, bootm 0x81000000).
#   modules.tar.gz, config, System.map
#
#   KERNEL_TARBALL  kernel.org linux-$KVER.tar.xz (default ~/kbuild6/linux-$KVER.tar.xz)
#   INITRAMFS_MODULES=1  put every module in the initramfs image too (about 1 MB more: each
#                   1 MB costs about 9 minutes with tools/uboot_mm_load.py)
#   BUSYBOX         static MIPS BusyBox (default payload/bin/busybox from build/build_busybox.sh)
#   HOST_TOOLS      optional prefix with flex, bison, m4 and mkimage (usr/bin, usr/share/bison)
#                   for a build host without them installed (apt-get download + dpkg -x)
#
# Host needs: mips-linux-gnu-gcc (gcc-mips-linux-gnu), flex, bison, bc, xz, mkimage (u-boot-tools).
set -euo pipefail

KVER="${KVER:-6.18.65}"
KSHA256="c92606f956f09dc50b5a0e2af9e86763fce27ada1647de97f8d1ac8cfc95801d"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$REPO_ROOT/build/kernel6"
KERNEL_TARBALL="${KERNEL_TARBALL:-$HOME/kbuild6/linux-$KVER.tar.xz}"
BUSYBOX="${BUSYBOX:-$REPO_ROOT/payload/bin/busybox}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build/output/kernel6}"
WORK="${WORK:-$HOME/kbuild6/work}"
DTS=ar9330_logitech_harmony-hub
LOADADDR=0x80060000
PART_SIZE=$((0xF0000))

if [ -n "${HOST_TOOLS:-}" ]; then
    export PATH="$HOST_TOOLS/usr/bin:$PATH"
    export BISON_PKGDATADIR="$HOST_TOOLS/usr/share/bison" M4="$HOST_TOOLS/usr/bin/m4"
fi
for t in mips-linux-gnu-gcc flex bison bc xz mkimage; do
    command -v "$t" >/dev/null || { echo "Error: $t not found" >&2; exit 1; }
done
for f in "$KERNEL_TARBALL" "$BUSYBOX"; do
    [ -e "$f" ] || { echo "Error: $f not found" >&2; exit 1; }
done
[ "$KVER" != 6.18.65 ] || echo "$KSHA256  $KERNEL_TARBALL" | sha256sum -c --quiet - \
    || { echo "Error: $KERNEL_TARBALL is not the expected file" >&2; exit 1; }

MAKE=(make -j"$(nproc)" ARCH=mips CROSS_COMPILE=mips-linux-gnu-)

echo "=== Unpacking linux-$KVER to $WORK ==="
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT_DIR"
tar -xJf "$KERNEL_TARBALL" -C "$WORK" --strip-components=1
cd "$WORK"
tr -d '\r' < "$SRC/$DTS.dts" > "arch/mips/boot/dts/qca/$DTS.dts"  # a Windows checkout has CRLF
echo "dtb-\$(CONFIG_ATH79) += $DTS.dtb" >> arch/mips/boot/dts/qca/Makefile

echo "=== Configuring (ath79_defconfig + harmony_hub.config) ==="
tr -d '\r' < "$SRC/harmony_hub.config" > harmony_hub.config
"${MAKE[@]}" ath79_defconfig > /dev/null
scripts/kconfig/merge_config.sh -m .config harmony_hub.config > merge.log
"${MAKE[@]}" olddefconfig > /dev/null
# every option of the fragment must survive olddefconfig (a typo or a missing dependency drops it)
missing=0
while IFS= read -r line; do
    case "$line" in
        CONFIG_*=*) grep -qxF "$line" .config || { echo "Error: $line not in .config" >&2; missing=1; } ;;
        "# CONFIG_"*" is not set") opt="${line#\# }"; opt="${opt%% *}"
            if grep -q "^$opt=" .config; then echo "Error: $opt is still on" >&2; missing=1; fi ;;
    esac
done < harmony_hub.config
[ "$missing" = 0 ] || exit 1

# vmlinux.bin + DTB, LZMA with the stock image's header (props 0x5d, 8 MB dictionary, known
# size: the Atheros U-Boot 1.1.4 decoder reads the size from the header), then a uImage
make_uimage() {  # $1 output, $2 name
    cat arch/mips/boot/vmlinux.bin "arch/mips/boot/dts/qca/$DTS.dtb" > kernel-dtb.bin
    xz --format=lzma --lzma1=preset=9e,dict=8MiB -c kernel-dtb.bin > kernel-dtb.lzma
    python3 - kernel-dtb.bin kernel-dtb.lzma <<'EOF'
import os, struct, sys
size = os.path.getsize(sys.argv[1])
with open(sys.argv[2], 'r+b') as f:
    f.seek(5)
    f.write(struct.pack('<Q', size))
EOF
    local entry
    entry="0x$(awk '$3 == "kernel_entry" { print $1 }' System.map)"
    mkimage -A mips -O linux -T kernel -C lzma -a "$LOADADDR" -e "$entry" -n "$2" \
        -d kernel-dtb.lzma "$1" > /dev/null
}

echo "=== Building kernel, modules, DTB ==="
"${MAKE[@]}" vmlinux.bin modules dtbs > build.log 2>&1 || { grep -n -m 20 -i "error" build.log >&2; exit 1; }
make_uimage "$OUT_DIR/harmony-hub-kernel.uImage" "Linux-$KVER-harmony"
cp .config "$OUT_DIR/config"
cp System.map "$OUT_DIR/System.map"
rm -rf modroot
"${MAKE[@]}" INSTALL_MOD_PATH="$WORK/modroot" INSTALL_MOD_STRIP=1 DEPMOD=/sbin/depmod modules_install > /dev/null
tar -czf "$OUT_DIR/modules.tar.gz" -C modroot lib

echo "=== Building the initramfs test image ==="
KREL="$(cat include/config/kernel.release)"
{
    for d in /bin /dev /proc /sys /tmp /lib /lib/modules; do echo "dir $d 755 0 0"; done
    echo "nod /dev/console 600 0 0 c 5 1"
    echo "file /bin/busybox $BUSYBOX 755 0 0"
    echo "slink /bin/sh busybox 777 0 0"
    echo "file /init $WORK/initramfs-init 755 0 0"
    if [ "${INITRAMFS_MODULES:-0}" = 1 ]; then
        (cd modroot && find "lib/modules/$KREL" -type d | sort | sed 's|^|dir /|; s|$| 755 0 0|')
        (cd modroot && find "lib/modules/$KREL" -type f | sort | sed "s|.*|file /& $WORK/modroot/& 644 0 0|")
    fi
} > initramfs.list
tr -d '\r' < "$SRC/initramfs_init" > initramfs-init
./scripts/config --set-str INITRAMFS_SOURCE "$WORK/initramfs.list" -e INITRAMFS_COMPRESSION_XZ
"${MAKE[@]}" olddefconfig > /dev/null
"${MAKE[@]}" vmlinux.bin > build-initramfs.log 2>&1 || { grep -n -m 20 -i "error" build-initramfs.log >&2; exit 1; }
make_uimage "$OUT_DIR/harmony-hub-initramfs.uImage" "Linux-$KVER-harmony-initramfs"

echo "=== Result ==="
size=$(stat -c %s "$OUT_DIR/harmony-hub-kernel.uImage")
echo "harmony-hub-kernel.uImage: $size bytes (partition $PART_SIZE)"
if [ "$size" -le "$PART_SIZE" ]; then
    echo "Fits kernel/kernel2 with $((PART_SIZE - size)) bytes free"
else
    echo "Does NOT fit kernel/kernel2: $((size - PART_SIZE)) bytes too big"
fi
echo "harmony-hub-initramfs.uImage: $(stat -c %s "$OUT_DIR/harmony-hub-initramfs.uImage") bytes (RAM boot only)"
