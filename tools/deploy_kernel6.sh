#!/bin/bash
# Install the Linux 6.18 Hub system files on a Hub that runs it, over SSH, and write its kernel:
#
#   /cache/kernel6/rc.sh, udhcpc.sh, modules/   (build/kernel6, kernel6-modules.tgz)
#   /mnt/data/kernel6/bin                       (wpa_supplicant, wpa_cli, bccmd, hciattach, hciconfig)
#   /mnt/data/kernel6/firmware                  (CSR8811 PSKEY files from the stock firmware)
#   kernel + kernel2 partitions                 (harmony-hub-flash.uImage, hub_util flash-image)
#
#   tools/deploy_kernel6.sh <hub-ip> [--no-flash] [--reboot]
#
#   OUT        build output (default build/output/kernel6: build_kernel6.sh, build_wpa_supplicant.sh,
#              build_bluez_tools.sh)
#   STOCK_ROOT extracted stock root file system, for lib/firmware/csr8811-*.psr
#   SSH_KEY    private key for root (default: ssh's own choice)
set -euo pipefail

HOST="${1:?usage: $0 <hub-ip> [--no-flash] [--reboot]}"
shift
FLASH=1
REBOOT=0
for a in "$@"; do
    case "$a" in
        --no-flash) FLASH=0 ;;
        --reboot) REBOOT=1 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$REPO/build/output/kernel6}"
STOCK_ROOT="${STOCK_ROOT:-$REPO/background/hub_firmware_extracted/squashfs-root}"
export MSYS_NO_PATHCONV=1   # Git Bash: keep /tmp/... remote paths as they are
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR ${SSH_KEY:+-i "$SSH_KEY"} "root@$HOST")

need() { [ -f "$1" ] || { echo "Error: $1 not found" >&2; exit 1; }; }
for f in harmony-hub-flash.uImage kernel6-modules.tgz bin/wpa_supplicant bin/wpa_cli bin/bccmd \
         bin/hciattach bin/hciconfig; do
    need "$OUT/$f"
done
need "$REPO/payload/bin/hub_util"
for p in csr8811-a06.psr csr8811-a08.psr csr8811-a08-pb1.psr; do
    need "$STOCK_ROOT/lib/firmware/$p"
done

push() { "${SSH[@]}" "cat > $2" < "$1"; }   # local file, remote path

"${SSH[@]}" 'uname -r | grep -q harmony' || { echo "Error: $HOST does not run the 6.18 system" >&2; exit 1; }
echo "=== Copying to $HOST:/tmp/k6 ==="
"${SSH[@]}" 'rm -rf /tmp/k6 && mkdir -p /tmp/k6/bin /tmp/k6/firmware'
push "$OUT/harmony-hub-flash.uImage" /tmp/k6/flash.uImage
push "$OUT/kernel6-modules.tgz" /tmp/k6/modules.tgz
push "$REPO/payload/bin/hub_util" /tmp/k6/hub_util
for f in rc.sh udhcpc.sh modprobe.sh usb_gadget.sh; do
    tr -d '\r' < "$REPO/build/kernel6/$f" > "$OUT/$f.lf"
    push "$OUT/$f.lf" "/tmp/k6/$f"
done
for b in wpa_supplicant wpa_cli bccmd hciattach hciconfig; do
    push "$OUT/bin/$b" "/tmp/k6/bin/$b"
done
for p in csr8811-a06.psr csr8811-a08.psr csr8811-a08-pb1.psr; do
    push "$STOCK_ROOT/lib/firmware/$p" "/tmp/k6/firmware/$p"
done

echo "=== Installing ==="
"${SSH[@]}" "set -e
cd /tmp/k6
chmod 755 hub_util bin/* rc.sh udhcpc.sh modprobe.sh usb_gadget.sh
rm -rf /cache/kernel6/modules && mkdir -p /cache/kernel6/modules
tar -xzf modules.tgz -C /cache/kernel6/modules
cp rc.sh udhcpc.sh modprobe.sh usb_gadget.sh /cache/kernel6/
mkdir -p /mnt/data/kernel6
rm -rf /mnt/data/kernel6/bin /mnt/data/kernel6/firmware /mnt/data/kernel6/net
cp -a bin firmware /mnt/data/kernel6/
cp hub_util /mnt/data/kernel6/bin/   # usbhid, mfg: newer than an older install has
sync
df -k /cache /mnt/data | tail -2"

if [ "$FLASH" = 1 ]; then
    echo "=== Writing the kernel ==="
    # the CC2544 shares the SPI bus with the flash: hub_rf may not run (hub_util refuses)
    "${SSH[@]}" 'killall -9 hub_rf hub_btstack 2>/dev/null; sleep 1; /tmp/k6/hub_util flash-image /tmp/k6/flash.uImage /dev/mtd1 /dev/mtd2 && sync'
fi
if [ "$REBOOT" = 1 ]; then
    echo "=== Rebooting ==="
    "${SSH[@]}" 'sync; mount -o remount,ro /mnt/data 2>/dev/null; sync; (sleep 1; echo b > /proc/sysrq-trigger) &'
fi
echo "Done."
