#!/bin/bash
# Build mke2fs from e2fsprogs for the Linux 6.18 Hub system (shared uClibc-ng), to format the USB
# stick that holds data and cache (build/kernel6/usb_storage.sh) as ext4 with a journal: the Hub
# has no shutdown, a power cut is its normal end, and BusyBox's mke2fs makes ext2 without one.
# Run in WSL/Linux.
#
# Output: $OUT_DIR/mke2fs (default build/output/kernel6/bin), for build/build_rootfs6.sh.
set -euo pipefail

E2_VER=1.47.1
E2_SHA256=5a33dc047fd47284bca4bb10c13cfe7896377ae3d01cb81a05d406025d99e0d1
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TC="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
WORK="${WORK:-$HOME/kbuild6/e2fsprogs}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build/output/kernel6/bin}"

[ -x "${TC}gcc" ] || { echo "Error: ${TC}gcc not found" >&2; exit 1; }
mkdir -p "$WORK" "$OUT_DIR"
cd "$WORK"
[ -f "e2fsprogs-$E2_VER.tar.xz" ] ||
    curl -fsSLO "https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v$E2_VER/e2fsprogs-$E2_VER.tar.xz"
echo "$E2_SHA256  e2fsprogs-$E2_VER.tar.xz" | sha256sum -c --quiet -
rm -rf "e2fsprogs-$E2_VER"
tar -xJf "e2fsprogs-$E2_VER.tar.xz"
cd "e2fsprogs-$E2_VER"

# libext2fs and friends linked in (only mke2fs uses them), libc shared
./configure --host=mips-buildroot-linux-uclibc CC="${TC}gcc" AR="${TC}ar" RANLIB="${TC}ranlib" \
    CFLAGS="-Os -ffunction-sections -fdata-sections" LDFLAGS="-Wl,--gc-sections" \
    --disable-nls --disable-fuse2fs --disable-e2initrd-helper --disable-debugfs --disable-imager \
    --disable-resizer --disable-defrag --disable-tls --disable-uuidd --disable-fsck \
    --disable-elf-shlibs --disable-tdb --without-libarchive --without-udev-rules-dir \
    --without-crond-dir --without-systemd-unit-dir > configure.log 2>&1 \
    || { tail -20 configure.log; exit 1; }
make -j"$(nproc)" libs > build.log 2>&1 || { tail -30 build.log; exit 1; }
make -C misc -j"$(nproc)" mke2fs >> build.log 2>&1 || { tail -30 build.log; exit 1; }
"${TC}strip" -o "$OUT_DIR/mke2fs" misc/mke2fs
echo "mke2fs: $(stat -c %s "$OUT_DIR/mke2fs") bytes"
