#!/bin/bash
# Build the root file system of the Linux 6.18 Hub system: an XZ squashfs for the stock root
# partition (mtd "root", 4 MB). Run in WSL/Linux after build_kernel6.sh and the dynamic builds:
#
#   sh build/compile_all.sh util                      hub_util (system tool: usbhid, mfg skin)
#   bash build/build_busybox.sh mips
#   bash build/build_minimal_dropbear.sh
#   bash build/build_wpa_supplicant.sh
#   bash build/build_bluez_tools.sh
#   bash build/build_e2fsprogs.sh
#
# The kernel's init (build/kernel6/init.c) mounts it as the lower layer under the data partition
# (overlayfs), so the stock layout (/data/harmony, /etc, /home/root) comes from the data partition
# as on the stock kernel, and HarmonyNG installs and updates there as before. This image holds the
# system: shared uClibc-ng, BusyBox, dropbear, wpa_supplicant, the BlueZ tools, the kernel modules
# and the boot scripts. Not in it: the CSR8811 PSKEY files (stock firmware; /lib/firmware points
# to /kernel6/firmware in the data partition, put there by tools/install_kernel6.py).
#
# Output: $OUT/harmony-hub-rootfs.sqfs (fails over 4 MB). Installed by tools/install_kernel6.py
# --rootfs (the fallback boot writes it: build/kernel6/rc.sh).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$REPO_ROOT/build/kernel6"
OUT="${OUT:-$REPO_ROOT/build/output/kernel6}"
DYN="$REPO_ROOT/build/output/kernel6/bin"   # system tools (busybox, dropbear, wpa_supplicant, BlueZ)
HNG="$REPO_ROOT/payload/bin"                # HarmonyNG binaries (hub_util)
SYSROOT="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/mips-buildroot-linux-uclibc/sysroot"
CROSS="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
BB_BUILD="$REPO_ROOT/build/tmp/busybox-1.36.1-mips"
ROOT_SIZE=$((0x400000))
STAGE="${STAGE:-$HOME/kbuild6/rootfs}"

if [ -n "${HOST_TOOLS:-}" ]; then
    export PATH="$HOST_TOOLS/usr/bin:$PATH"
fi
command -v mksquashfs >/dev/null || { echo "Error: mksquashfs not found (squashfs-tools)" >&2; exit 1; }
for f in "$DYN/busybox" "$DYN/dropbearmulti" "$DYN/wpa_supplicant" "$DYN/wpa_cli" "$DYN/bccmd"          "$DYN/hciattach" "$DYN/hciconfig" "$DYN/mke2fs" "$HNG/hub_util"; do
    [ -f "$f" ] || { echo "Error: $f not built (see the header)" >&2; exit 1; }
done
[ -f "$OUT/kernel6-modules.tgz" ] || { echo "Error: $OUT/kernel6-modules.tgz missing (build_kernel6.sh)" >&2; exit 1; }
[ -f "$BB_BUILD/.config" ] || { echo "Error: $BB_BUILD missing (build_busybox.sh mips)" >&2; exit 1; }

echo "=== Staging in $STAGE ==="
rm -rf "$STAGE"
mkdir -p "$STAGE"
cd "$STAGE"
mkdir -p bin sbin lib usr/bin usr/sbin usr/lib/kernel6/bin usr/share/udhcpc etc/init.d \
    proc sys dev tmp cache rom overlay mnt usb flash data home/root root var/volatile lib/modules
chmod 1777 tmp
chmod 700 home/root root
for d in run log lock tmp; do ln -s volatile/$d var/$d; done

# C library: the loader under the stock name (the plugins and hub_fetch ask for ld-uClibc.so.0)
cp -a "$SYSROOT"/lib/ld-uClibc* "$SYSROOT"/lib/libuClibc-1.0.24.so "$SYSROOT"/lib/libc.so.0 \
    "$SYSROOT"/lib/libc.so.1 lib/
cp "$SYSROOT/lib/libgcc_s.so.1" lib/ && "${CROSS}strip" lib/libgcc_s.so.1

# BusyBox and its applets (paths from its build)
cp "$DYN/busybox" bin/busybox
(cd "$BB_BUILD" && make -s busybox.links > /dev/null 2>&1 || true)
while read -r a; do
    [ -n "$a" ] && [ "$a" != /bin/busybox ] || continue
    rel=$(echo "${a#/}" | sed 's|[^/]*/|../|g; s|[^/]*$||')
    ln -sf "${rel}bin/busybox" "${a#/}"
done < "$BB_BUILD/busybox.links"
[ -e bin/sh ] || ln -s busybox bin/sh

# dropbear: hub_sshd (HarmonyNG) finds it as /usr/sbin/dropbearmulti
cp "$DYN/dropbearmulti" usr/sbin/
# the stock root's names: older HarmonyNG rcS.local files expect /usr/sbin/dropbear (rc.sh starts it)
for a in dropbear dropbearkey scp; do ln -s dropbearmulti usr/sbin/$a; done
# tools of the system, under the stock names where the stock scripts call them. An applet link
# of the same name goes first: writing through it would overwrite bin/busybox.
rm -f usr/sbin/wpa_supplicant usr/sbin/wpa_cli sbin/wlanconfig sbin/modprobe
for t in wpa_supplicant wpa_cli bccmd hciattach hciconfig mke2fs; do cp "$DYN/$t" usr/lib/kernel6/bin/; done
ln -s ../lib/kernel6/bin/mke2fs usr/sbin/mkfs.ext4   # ext4 with a journal (BusyBox: ext2)
cp "$HNG/hub_util" usr/lib/kernel6/bin/
printf '#!/bin/sh\nexec /usr/lib/kernel6/bin/wpa_supplicant -D nl80211 "$@"\n' > usr/sbin/wpa_supplicant
printf '#!/bin/sh\nexit 0\n' > sbin/wlanconfig
ln -s ../lib/kernel6/bin/wpa_cli usr/sbin/wpa_cli
chmod 755 usr/sbin/wpa_supplicant sbin/wlanconfig

# boot and helper scripts
for f in rc.sh modprobe.sh udhcpc.sh usb_gadget.sh usb_storage.sh; do
    tr -d '\r' < "$SRC/$f" > "usr/lib/kernel6/$f"
    chmod 755 "usr/lib/kernel6/$f"
done
ln -s ../../usr/lib/kernel6/rc.sh etc/init.d/rcS
cp usr/lib/kernel6/modprobe.sh sbin/modprobe
ln -sf ../../lib/kernel6/udhcpc.sh usr/share/udhcpc/default.script

# kernel modules; PSKEY files from the data partition
tar -xzf "$OUT/kernel6-modules.tgz" -C lib/modules
ln -s /kernel6/firmware lib/firmware

# base /etc (the data partition's /etc, the stock one, has the Hub's own files on top)
echo 'root::0:0:root:/home/root:/bin/sh' > etc/passwd
echo 'root:x:0:' > etc/group
echo HarmonyHub > etc/hostname
# the system version the web UI shows as "Firmware" (hub_info_firmware): kernel release + build day
echo "$(ls lib/modules | sed -n 1p) ($(date -u +%Y-%m-%d))" > etc/version
echo '127.0.0.1 localhost' > etc/hosts
echo /bin/sh > etc/shells
printf 'export PATH=/data/harmony/bin:/cache/bin:/usr/sbin:/usr/bin:/sbin:/bin\nexport PS1="\\h:\\w\\$ "\n' > etc/profile
printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n' > etc/wpa_supplicant.conf

echo "=== Checking the binaries' libraries ==="
for f in bin/busybox usr/sbin/dropbearmulti usr/lib/kernel6/bin/*; do
    [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ] || { echo "Error: $f is not an ELF binary" >&2; exit 1; }
    for lib in $("${CROSS}readelf" -d "$f" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
        [ -e "lib/$lib" ] || { echo "Error: $f needs $lib, not in the image" >&2; exit 1; }
    done
done

echo "=== mksquashfs ==="
mkdir -p "$OUT"
rm -f "$OUT/harmony-hub-rootfs.sqfs"
mksquashfs "$STAGE" "$OUT/harmony-hub-rootfs.sqfs" -comp xz -b 256K -all-root -no-xattrs -noappend -quiet
size=$(stat -c %s "$OUT/harmony-hub-rootfs.sqfs")
echo "harmony-hub-rootfs.sqfs: $size bytes, $((ROOT_SIZE - size)) of the $ROOT_SIZE byte root partition free"
[ "$size" -le "$ROOT_SIZE" ] || { echo "Error: the image does not fit the root partition" >&2; exit 1; }
