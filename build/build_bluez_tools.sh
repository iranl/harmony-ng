#!/bin/bash
# Build bccmd, hciattach and hciconfig from BlueZ 5.55 (the last release with bccmd) as static
# MIPS binaries for the Linux 6.18 Hub system. The stock firmware's copies are linked against the
# stock root's uClibc 0.9.30, which the new system does not mount. Run in WSL/Linux.
#
#   bccmd      PSKEY load of the CSR8811 over BCSP (patches, BT address), buildname
#   hciattach  BCSP link set-up on the UART and the kernel's hci_uart line discipline
#   hciconfig  hci0 up/down
#
# Output: $OUT_DIR (default build/output/kernel6/bin); on the Hub /mnt/data/kernel6/bin.
set -euo pipefail

BZ_VER=5.55
BZ_SHA256=8863717113c4897e2ad3271fc808ea245319e6fd95eed2e934fae8e0894e9b88
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TC="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
WORK="${WORK:-$HOME/kbuild6/bluez}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build/output/kernel6/bin}"
# LINK=dynamic: against the toolchain's shared uClibc-ng, for the Linux 6.18 root file system
# (build/build_rootfs6.sh), into build/output/kernel6/harmony/bin
STATIC=-static
if [ "${LINK:-static}" = dynamic ]; then
    STATIC=""
    OUT_DIR="$REPO_ROOT/build/output/kernel6/harmony/bin"
fi

[ -x "${TC}gcc" ] || { echo "Error: ${TC}gcc not found" >&2; exit 1; }
mkdir -p "$WORK" "$OUT_DIR"
cd "$WORK"
[ -f "bluez-$BZ_VER.tar.xz" ] || curl -fsSLO "https://www.kernel.org/pub/linux/bluetooth/bluez-$BZ_VER.tar.xz"
echo "$BZ_SHA256  bluez-$BZ_VER.tar.xz" | sha256sum -c --quiet -
rm -rf "bluez-$BZ_VER"
tar -xJf "bluez-$BZ_VER.tar.xz"
cd "bluez-$BZ_VER"

# <bluetooth/*.h> are lib/*.h; config.h holds what configure would define
mkdir -p inc/bluetooth
cp lib/*.h inc/bluetooth/
printf '#define VERSION "%s"\n#ifndef _GNU_SOURCE\n#define _GNU_SOURCE 1\n#endif\n' "$BZ_VER" > inc/config.h
LIB="lib/bluetooth.c lib/hci.c"
cc() {
    local out=$1
    shift
    "${TC}gcc" -Os $STATIC -Wno-format-truncation -I. -Iinc -Ilib -include config.h \
        -o "$out" "$@" $LIB
    "${TC}strip" "$out"
}

cc bccmd tools/bccmd.c tools/csr.c tools/csr_hci.c tools/csr_usb.c tools/csr_h4.c \
    tools/csr_3wire.c tools/csr_bcsp.c tools/ubcsp.c
cc hciattach tools/hciattach.c tools/hciattach_st.c tools/hciattach_ti.c tools/hciattach_tialt.c \
    tools/hciattach_ath3k.c tools/hciattach_qualcomm.c tools/hciattach_intel.c \
    tools/hciattach_bcm43xx.c
cc hciconfig tools/hciconfig.c tools/csr.c

cp bccmd hciattach hciconfig "$OUT_DIR/"
ls -la "$OUT_DIR/bccmd" "$OUT_DIR/hciattach" "$OUT_DIR/hciconfig"
