#!/bin/bash
# Install the Linux 6.18 Hub system on a Hub that runs it, over SSH.
#
#   tools/deploy_kernel6.sh <hub-ip> [--rootfs] [--no-flash] [--reboot]
#
# Always:
#   kernel + kernel2 partitions   harmony-hub-flash.uImage (hub_util flash-image; --no-flash skips)
#   /cache/kernel6                the fallback boot's scripts (rc.sh, modprobe.sh, udhcpc.sh,
#                                 usb_gadget.sh): init falls back to them without a root file
#                                 system, and to write a new one
#   <data>/kernel6/bin/hub_util   for the fallback boot (flash-image, usbhid, mfg)
#   <data>/kernel6/firmware       the CSR8811 PSKEY files from the stock firmware
#     (<data>: /mnt/data on a fallback boot, the overlay's /kernel6 on a root file system boot)
# --rootfs: harmony-hub-rootfs.sqfs (build_rootfs6.sh) to the root partition: written at once on
#   a fallback boot (the root partition is not mounted), or left in /cache/kernel6/rootfs.sqfs for
#   the next boot on a root file system boot (init boots the fallback, which writes it).
# Without --rootfs on a fallback boot: also its modules and tools (/cache/kernel6/modules,
#   /mnt/data/kernel6/bin), as before there was a root file system.
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
        --rootfs) ROOTFS=1 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$REPO/build/output/kernel6}"
STOCK_ROOT="${STOCK_ROOT:-$REPO/background/hub_firmware_extracted/squashfs-root}"
export MSYS_NO_PATHCONV=1   # Git Bash: keep /tmp/... remote paths as they are
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR ${SSH_KEY:+-i "$SSH_KEY"} "root@$HOST")
PSR="csr8811-a06.psr csr8811-a08.psr csr8811-a08-pb1.psr"

need() { [ -f "$1" ] || { echo "Error: $1 not found" >&2; exit 1; }; }
push() { "${SSH[@]}" "cat > $2" < "$1"; }   # local file, remote path

need "$OUT/harmony-hub-flash.uImage"
need "$REPO/payload/bin/hub_util"
for p in $PSR; do need "$STOCK_ROOT/lib/firmware/$p"; done
[ "$ROOTFS" = 0 ] || need "$OUT/harmony-hub-rootfs.sqfs"

"${SSH[@]}" 'uname -r | grep -q harmony' || { echo "Error: $HOST does not run the 6.18 system" >&2; exit 1; }
if "${SSH[@]}" "grep -q '^overlay / overlay' /proc/mounts"; then
    BOOT=rootfs
    DATA=/kernel6
else
    BOOT=fallback
    DATA=/mnt/data/kernel6
    if "${SSH[@]}" '[ -d /mnt/data/upper ]'; then DATA=/mnt/data/upper/kernel6; fi
fi
echo "=== $HOST: $BOOT boot ==="
if [ "$BOOT" = fallback ] && [ "$ROOTFS" = 0 ]; then
    for f in kernel6-modules.tgz bin/wpa_supplicant bin/wpa_cli bin/bccmd bin/hciattach bin/hciconfig; do
        need "$OUT/$f"
    done
fi

echo "=== Copying to $HOST:/tmp/k6 ==="
"${SSH[@]}" 'rm -rf /tmp/k6 && mkdir -p /tmp/k6/bin /tmp/k6/firmware'
push "$OUT/harmony-hub-flash.uImage" /tmp/k6/flash.uImage
push "$REPO/payload/bin/hub_util" /tmp/k6/hub_util
for f in rc.sh udhcpc.sh modprobe.sh usb_gadget.sh; do
    tr -d '\r' < "$REPO/build/kernel6/$f" > "$OUT/$f.lf"
    push "$OUT/$f.lf" "/tmp/k6/$f"
done
for p in $PSR; do push "$STOCK_ROOT/lib/firmware/$p" "/tmp/k6/firmware/$p"; done
[ "$ROOTFS" = 0 ] || push "$OUT/harmony-hub-rootfs.sqfs" /tmp/k6/rootfs.sqfs
if [ "$BOOT" = fallback ] && [ "$ROOTFS" = 0 ]; then
    push "$OUT/kernel6-modules.tgz" /tmp/k6/modules.tgz
    for b in wpa_supplicant wpa_cli bccmd hciattach hciconfig; do push "$OUT/bin/$b" "/tmp/k6/bin/$b"; done
fi

echo "=== Installing ==="
"${SSH[@]}" "set -e
cd /tmp/k6
chmod 755 hub_util rc.sh udhcpc.sh modprobe.sh usb_gadget.sh
mkdir -p /cache/kernel6 $DATA/bin $DATA/firmware
cp rc.sh udhcpc.sh modprobe.sh usb_gadget.sh /cache/kernel6/
cp hub_util $DATA/bin/
cp firmware/* $DATA/firmware/
if [ -f modules.tgz ]; then
    rm -rf /cache/kernel6/modules && mkdir -p /cache/kernel6/modules
    tar -xzf modules.tgz -C /cache/kernel6/modules
    chmod 755 bin/*
    cp bin/* $DATA/bin/
fi
sync
df -k /cache /mnt/data /overlay 2>/dev/null | grep -v '^Filesystem' || true"

if [ "$FLASH" = 1 ] || [ "$ROOTFS" = 1 ]; then
    # the CC2544 shares the SPI bus with the flash: hub_rf may not run (hub_util refuses)
    "${SSH[@]}" 'killall -9 hub_rf hub_btstack 2>/dev/null; sleep 1; true'
fi
if [ "$FLASH" = 1 ]; then
    echo "=== Writing the kernel ==="
    "${SSH[@]}" '/tmp/k6/hub_util flash-image /tmp/k6/flash.uImage /dev/mtd1 /dev/mtd2 && sync'
fi
if [ "$ROOTFS" = 1 ]; then
    if [ "$BOOT" = fallback ]; then
        echo "=== Writing the root file system ==="
        "${SSH[@]}" 'n=$(grep "\"root\"" /proc/mtd | cut -d: -f1); /tmp/k6/hub_util flash-image /tmp/k6/rootfs.sqfs /dev/$n && sync'
    else
        echo "=== Root file system for the next boot (/cache/kernel6/rootfs.sqfs) ==="
        # the fallback boot's modules are not needed to write it: their space goes to the image
        "${SSH[@]}" 'rm -rf /cache/kernel6/modules; cp /tmp/k6/rootfs.sqfs /cache/kernel6/rootfs.sqfs && sync'
        REBOOT=1
    fi
fi
if [ "$REBOOT" = 1 ]; then
    echo "=== Rebooting ==="
    "${SSH[@]}" 'sync; (sleep 1; echo b > /proc/sysrq-trigger) &'
fi
echo "Done."
