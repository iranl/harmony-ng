#!/bin/bash
# Put a Hub on the Linux 6.18 system back on the stock kernel and root file system, over SSH,
# from the backup tools/migrate_to_kernel6.sh made:
#
#   tools/rollback_kernel6.sh <hub-ip> <backup-dir>
#
# The backup's kernel + kernel2 are written back byte for byte (the stock uImage at the start
# keeps its CRC check valid; kernel2 is U-Boot's fallback kernel), the stock root file system
# (--stock: its LZMA squashfs) over the 6.18 one, and the data partition gets the stock union
# layout back (upper/ to its top, the overlay's work dir removed): the stock kernel finds the
# Hub's HarmonyNG and settings as before the migration.
#
# The root partition is the running system's: the static rescue hub_util (LINK=static sh
# build/compile_all.sh util) runs from RAM (/tmp), writes it last with --force and the Hub
# restarts at once. Do not cut the power until the Hub is back on the
# stock firmware (a cut write leaves a Hub that needs the serial console).
#   SSH_KEY  private key for root (default: ssh's own choice)
set -euo pipefail

HOST="${1:?usage: $0 <hub-ip> <backup-dir>}"
BK="${2:?usage: $0 <hub-ip> <backup-dir>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
export MSYS_NO_PATHCONV=1
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR ${SSH_KEY:+-i "$SSH_KEY"} "root@$HOST")
push() { "${SSH[@]}" "cat > $2" < "$1"; }

for f in "$BK/kernel.bin" "$BK/kernel2.bin" "$BK/root.bin" "$REPO/build/output/kernel6/static/hub_util"; do
    [ -f "$f" ] || { echo "Error: $f not found" >&2; exit 1; }
done
head -c 4 "$BK/root.bin" | grep -q hsqs || { echo "Error: $BK/root.bin is not a squashfs" >&2; exit 1; }
"${SSH[@]}" 'uname -r | grep -q harmony' || { echo "Error: $HOST does not run the 6.18 system" >&2; exit 1; }
if "${SSH[@]}" "grep -q '^overlay / overlay' /proc/mounts"; then
    DATA=/overlay
else
    DATA=/mnt/data
fi

echo "=== Copying the stock images to $HOST:/tmp ==="
"${SSH[@]}" 'rm -rf /tmp/rb && mkdir -p /tmp/rb'
cat "$BK/kernel.bin" "$BK/kernel2.bin" > "$BK/kernels.bin"
push "$BK/kernels.bin" /tmp/rb/kernels.bin
push "$BK/root.bin" /tmp/rb/root.bin
push "$REPO/build/output/kernel6/static/hub_util" /tmp/rb/hub_util
for f in kernels.bin root.bin; do
    [ "$("${SSH[@]}" "md5sum /tmp/rb/$f | cut -d' ' -f1")" = "$(md5sum < "$BK/$f" | cut -d' ' -f1)" ] ||
        { echo "Error: /tmp/rb/$f does not match the backup" >&2; exit 1; }
done

echo "=== Writing the stock system ==="
"${SSH[@]}" "set -e
chmod 755 /tmp/rb/hub_util
/tmp/rb/hub_util flash-image --dry-run --stock /tmp/rb/root.bin /dev/\$(grep '\"root\"' /proc/mtd | cut -d: -f1) >/dev/null
touch /tmp/harmony_stop
killall -9 hub_rf hub_btstack hub_daemon 2>/dev/null || true
sleep 2
# the stock union layout back on the data partition
if [ -d $DATA/upper ]; then
    for e in $DATA/upper/* $DATA/upper/.[!.]*; do [ -e \"\$e\" ] && mv -f \"\$e\" $DATA/; done
    rmdir $DATA/upper
    rm -rf $DATA/work || true   # in use by the overlay mount: harmless for the stock kernel
fi
sync
root=/dev/\$(grep '\"root\"' /proc/mtd | cut -d: -f1)
/tmp/rb/hub_util flash-image /tmp/rb/kernels.bin /dev/mtd1 /dev/mtd2
# last: once the running root is overwritten, only what is in RAM still works
/tmp/rb/hub_util flash-image --stock --force /tmp/rb/root.bin \$root && echo b > /proc/sysrq-trigger"
echo "Done: the Hub restarts on the stock kernel."
