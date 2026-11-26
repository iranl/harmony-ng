#!/bin/bash
# Move a Hub that runs the stock kernel with HarmonyNG to the Linux 6.18 system, over SSH, without
# a serial console:
#
#   tools/migrate_to_kernel6.sh <hub-ip>
#
# 1. Backs up kernel, kernel2, root and mfg (the Hub's own calibration and MACs) to
#    backups/<hub-ip>-<date>/ and checks each copy's MD5 against the Hub.
# 2. Puts on the Hub: the CSR8811 PSKEY files from its own stock root (/kernel6/firmware), the
#    rescue boot's scripts (/cache/kernel6), and the root file system image waiting in
#    /cache/kernel6/rootfs.sqfs.
# 3. Writes the 6.18 kernel over kernel + kernel2 with the static rescue hub_util (the stock
#    uClibc 0.9.30 runs no uClibc-ng binary) and restarts.
# Then the Hub carries on alone: the new kernel's init writes the waiting image to the root
# partition and boots it; the first boot moves the data partition into the overlay and starts
# HarmonyNG. HarmonyNG's binaries must be the dynamic uClibc-ng builds (a release for 6.18).
# Back with tools/rollback_kernel6.sh and the backup.
#
# Needs in build/output/kernel6: harmony-hub-flash.uImage (build_kernel6.sh),
# harmony-hub-rootfs.sqfs (build_rootfs6.sh) and static/hub_util (LINK=static sh
# build/compile_all.sh util).
#   SSH_KEY  private key for root (default: ssh's own choice)
set -euo pipefail

HOST="${1:?usage: $0 <hub-ip>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$REPO/build/output/kernel6}"
export MSYS_NO_PATHCONV=1
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR ${SSH_KEY:+-i "$SSH_KEY"} "root@$HOST")
on_hub() { "${SSH[@]}" "export PATH=/cache/bin:\$PATH; $1"; }
push() { "${SSH[@]}" "cat > $2" < "$1"; }

for f in "$OUT/harmony-hub-flash.uImage" "$OUT/harmony-hub-rootfs.sqfs" "$OUT/static/hub_util"; do
    [ -f "$f" ] || { echo "Error: $f not found" >&2; exit 1; }
done

echo "=== Checking $HOST ==="
rel=$(on_hub 'uname -r')
case "$rel" in
    2.6.31*) ;;
    *harmony*) echo "Error: $HOST already runs the 6.18 system (tools/deploy_kernel6.sh updates it)" >&2; exit 1 ;;
    *) echo "Error: $HOST runs kernel $rel, not the stock 2.6.31" >&2; exit 1 ;;
esac
on_hub '[ -x /cache/bin/busybox ] && [ -d /data/harmony ]' ||
    { echo "Error: HarmonyNG is not installed (/cache/bin/busybox, /data/harmony)" >&2; exit 1; }
on_hub 'ls /lib/firmware/csr8811-a06.psr /lib/firmware/csr8811-a08.psr /lib/firmware/csr8811-a08-pb1.psr >/dev/null' ||
    { echo "Error: the stock PSKEY files are missing from /lib/firmware" >&2; exit 1; }
need=$(( $(stat -c %s "$OUT/harmony-hub-rootfs.sqfs") / 1024 + 128 ))
free=$(on_hub "df -k /cache | awk 'NR == 2 { print \$4 }'")
if [ "$free" -lt "$need" ]; then
    echo "Error: /cache has $free KB free, the root file system image needs $need KB." >&2
    echo "       Move the icons and plugins to a USB stick (System page, USB Storage) and run this again." >&2
    exit 1
fi

BK="$REPO/backups/$HOST-$(date +%Y%m%d-%H%M%S)"
echo "=== Backing up to $BK ==="
mkdir -p "$BK"
for part in kernel kernel2 root mfg; do
    n=$(on_hub "grep '\"$part\"' /proc/mtd | cut -d: -f1")
    "${SSH[@]}" "cat /dev/$n" > "$BK/$part.bin"
    remote=$(on_hub "md5sum /dev/$n | cut -d' ' -f1")
    local_md5=$(md5sum < "$BK/$part.bin" | cut -d' ' -f1)
    [ "$remote" = "$local_md5" ] || { echo "Error: the $part backup does not match the Hub" >&2; exit 1; }
    echo "  $part: $(stat -c %s "$BK/$part.bin") bytes, MD5 $local_md5"
done

echo "=== Installing the 6.18 system files ==="
on_hub 'rm -rf /tmp/k6 && mkdir -p /tmp/k6 /cache/kernel6 /kernel6/firmware'
push "$OUT/harmony-hub-flash.uImage" /tmp/k6/flash.uImage
push "$OUT/static/hub_util" /tmp/k6/hub_util
for f in rc.sh udhcpc.sh modprobe.sh usb_gadget.sh; do
    tr -d '\r' < "$REPO/build/kernel6/$f" > "$OUT/$f.lf"
    push "$OUT/$f.lf" "/cache/kernel6/$f"
done
push "$OUT/harmony-hub-rootfs.sqfs" /cache/kernel6/rootfs.sqfs.new
on_hub 'set -e
chmod 755 /tmp/k6/hub_util /cache/kernel6/*.sh
cp /lib/firmware/csr8811-a06.psr /lib/firmware/csr8811-a08.psr /lib/firmware/csr8811-a08-pb1.psr /kernel6/firmware/
mv -f /cache/kernel6/rootfs.sqfs.new /cache/kernel6/rootfs.sqfs
/data/harmony/bin/hub_sync 2>/dev/null || sync'
sq_md5=$(md5sum < "$OUT/harmony-hub-rootfs.sqfs" | cut -d' ' -f1)
[ "$(on_hub 'md5sum /cache/kernel6/rootfs.sqfs | cut -d" " -f1')" = "$sq_md5" ] ||
    { echo "Error: the root file system image on the Hub does not match" >&2; exit 1; }

echo "=== Writing the 6.18 kernel ==="
# the services stop first: the CC2544 shares the SPI bus with the flash (AGENTS.md)
on_hub 'touch /tmp/harmony_stop; for p in $(cat /tmp/harmony_supervisors.pid 2>/dev/null); do kill -9 $p; done
killall -15 hub_rf 2>/dev/null; sleep 1
killall -9 hub_rf hub_btstack hub_daemon hal luaworks 2>/dev/null; sleep 2
/tmp/k6/hub_util flash-image /tmp/k6/flash.uImage /dev/mtd1 /dev/mtd2'

echo "=== Restarting ==="
on_hub '/data/harmony/bin/hub_sync 2>/dev/null || sync; sleep 2
mount -o remount,ro /data 2>/dev/null; mount -o remount,ro /cache 2>/dev/null
/bin/sh -c true && (sleep 1; reboot -f) >/dev/null 2>&1 &'
cat <<EOF
Done. The Hub now writes its root file system at boot (a few minutes); HarmonyNG comes back
with its settings. Backup: $BK
EOF
