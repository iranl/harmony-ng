#!/bin/bash
# Install the Linux 6.18 Hub system on a Hub that runs it, over SSH.
#
#   tools/deploy_kernel6.sh <hub-ip> [--rootfs] [--no-flash] [--reboot]
#
#   kernel + kernel2   harmony-hub-flash.uImage, written with the static rescue hub_util
#                      (LINK=static sh build/compile_all.sh util; --no-flash skips)
#   /cache/kernel6     the rescue boot's scripts (rc.sh, modprobe.sh, udhcpc.sh, usb_gadget.sh)
#   /kernel6/firmware  the CSR8811 PSKEY files from the stock firmware (data partition)
# --rootfs: harmony-hub-rootfs.sqfs (build_rootfs6.sh) to /cache/kernel6/rootfs.sqfs and a
#   reboot: the kernel's init writes it to the root partition before it mounts it.
#
#   OUT        build output (default build/output/kernel6)
#   STOCK_ROOT extracted stock root file system, for lib/firmware/csr8811-*.psr
#   SSH_KEY    private key for root (default: ssh's own choice)
set -euo pipefail

HOST="${1:?usage: $0 <hub-ip> [--rootfs] [--no-flash] [--reboot]}"
shift
FLASH=1
REBOOT=0
ROOTFS=0
for a in "$@"; do
    case "$a" in
        --no-flash) FLASH=0 ;;
        --reboot) REBOOT=1 ;;
        --rootfs) ROOTFS=1; REBOOT=1 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$REPO/build/output/kernel6}"
STOCK_ROOT="${STOCK_ROOT:-$REPO/background/hub_firmware_extracted/squashfs-root}"
HUB_UTIL="$OUT/static/hub_util"
export MSYS_NO_PATHCONV=1   # Git Bash: keep /tmp/... remote paths as they are
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR ${SSH_KEY:+-i "$SSH_KEY"} "root@$HOST")
PSR="csr8811-a06.psr csr8811-a08.psr csr8811-a08-pb1.psr"

need() { [ -f "$1" ] || { echo "Error: $1 not found" >&2; exit 1; }; }
push() { "${SSH[@]}" "cat > $2" < "$1"; }   # local file, remote path

need "$OUT/harmony-hub-flash.uImage"
need "$HUB_UTIL"
for p in $PSR; do need "$STOCK_ROOT/lib/firmware/$p"; done
[ "$ROOTFS" = 0 ] || need "$OUT/harmony-hub-rootfs.sqfs"

"${SSH[@]}" 'uname -r | grep -q harmony' || { echo "Error: $HOST does not run the 6.18 system" >&2; exit 1; }
# the data partition's top as the root file system sees it: /, or /mnt/data(/upper) on a rescue boot
if "${SSH[@]}" "grep -q '^overlay / overlay' /proc/mounts"; then
    DATA=
elif "${SSH[@]}" '[ -d /mnt/data/upper ]'; then
    DATA=/mnt/data/upper
else
    DATA=/mnt/data
fi

echo "=== Copying to $HOST ==="
"${SSH[@]}" "rm -rf /tmp/k6 && mkdir -p /tmp/k6 /cache/kernel6 $DATA/kernel6/firmware"
push "$OUT/harmony-hub-flash.uImage" /tmp/k6/flash.uImage
push "$HUB_UTIL" /tmp/k6/hub_util
for f in rc.sh udhcpc.sh modprobe.sh usb_gadget.sh; do
    tr -d '\r' < "$REPO/build/kernel6/$f" > "$OUT/$f.lf"
    push "$OUT/$f.lf" "/cache/kernel6/$f"
done
for p in $PSR; do push "$STOCK_ROOT/lib/firmware/$p" "$DATA/kernel6/firmware/$p"; done
if [ "$ROOTFS" = 1 ]; then
    # the old rescue boot's modules are not needed: their space goes to the image
    "${SSH[@]}" 'rm -rf /cache/kernel6/modules /cache/kernel6/rootfs.sqfs.failed'
    push "$OUT/harmony-hub-rootfs.sqfs" /cache/kernel6/rootfs.sqfs.new
    sum=$(md5sum < "$OUT/harmony-hub-rootfs.sqfs" | cut -d' ' -f1)
    [ "$("${SSH[@]}" 'md5sum /cache/kernel6/rootfs.sqfs.new | cut -d" " -f1')" = "$sum" ] ||
        { echo "Error: the root file system image on the Hub does not match" >&2; exit 1; }
    "${SSH[@]}" 'mv -f /cache/kernel6/rootfs.sqfs.new /cache/kernel6/rootfs.sqfs'
fi
"${SSH[@]}" 'chmod 755 /tmp/k6/hub_util /cache/kernel6/*.sh; sync; df -k /cache | sed -n 2p'

if [ "$FLASH" = 1 ]; then
    echo "=== Writing the kernel ==="
    # the CC2544 shares the SPI bus with the flash: hub_rf may not run (hub_util refuses)
    "${SSH[@]}" 'killall -9 hub_rf hub_btstack 2>/dev/null; sleep 1
/tmp/k6/hub_util flash-image /tmp/k6/flash.uImage /dev/mtd1 /dev/mtd2 && sync'
fi
if [ "$REBOOT" = 1 ]; then
    echo "=== Rebooting ==="
    "${SSH[@]}" 'sync; (sleep 1; echo b > /proc/sysrq-trigger) &'
fi
echo "Done."
