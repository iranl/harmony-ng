#!/bin/bash
# Build wpa_supplicant and wpa_cli for the Linux 6.18 Hub system (nl80211 driver, WPA/WPA2-PSK,
# internal crypto) as static MIPS binaries on OpenWrt's libnl-tiny. Run in WSL/Linux.
# Output: $OUT_DIR/wpa_supplicant, $OUT_DIR/wpa_cli (default build/output/kernel6/bin); on the
# Hub they go to /mnt/data/kernel6/bin (build/kernel6/rc.sh).
#
# No SAE (WPA3): the internal crypto has no elliptic curves.
set -euo pipefail

WPA_VER=2.11
WPA_SHA256=912ea06f74e30a8e36fbb68064d6cdff218d8d591db0fc5d75dee6c81ac7fc0a
# libnl-tiny has no releases: a fixed commit of github.com/openwrt/libnl-tiny
NL_COMMIT="${NL_COMMIT:-40493a655d8caa2ccf5206dde1e733abe2920432}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TC="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
WORK="${WORK:-$HOME/kbuild6/wpa}"
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
[ -f "wpa_supplicant-$WPA_VER.tar.gz" ] || curl -fsSLO "https://w1.fi/releases/wpa_supplicant-$WPA_VER.tar.gz"
echo "$WPA_SHA256  wpa_supplicant-$WPA_VER.tar.gz" | sha256sum -c --quiet -
curl -fsSL -o libnl-tiny.tar.gz "https://github.com/openwrt/libnl-tiny/archive/$NL_COMMIT.tar.gz"

echo "=== libnl-tiny ==="
rm -rf libnl-tiny-* nl && mkdir nl
tar -xzf libnl-tiny.tar.gz
(cd libnl-tiny-*/ && for f in *.c; do
    "${TC}gcc" -Os -D_GNU_SOURCE -Iinclude -c "$f" -o "$WORK/nl/${f%.c}.o"
done && cp -r include "$WORK/nl/include")
"${TC}ar" rcs nl/libnl-tiny.a nl/*.o

echo "=== wpa_supplicant $WPA_VER ==="
rm -rf "wpa_supplicant-$WPA_VER"
tar -xzf "wpa_supplicant-$WPA_VER.tar.gz"
cd "wpa_supplicant-$WPA_VER/wpa_supplicant"
cat > .config <<EOF
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL_TINY=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_TLS=internal
CONFIG_CRYPTO=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_NO_CONFIG_BLOBS=y
CONFIG_NO_RANDOM_POOL=y
CONFIG_ELOOP_POLL=y
CONFIG_IEEE80211W=y
CONFIG_SMALL_PKG=y
EOF
make -j"$(nproc)" CC="${TC}gcc" EXTRA_CFLAGS="-Os -D_GNU_SOURCE -I$WORK/nl/include" \
    LDFLAGS="$STATIC -L$WORK/nl" wpa_supplicant wpa_cli > build.log 2>&1 \
    || { grep -n -m 20 -i "error\|undefined" build.log >&2; exit 1; }
"${TC}strip" wpa_supplicant wpa_cli
cp wpa_supplicant wpa_cli "$OUT_DIR/"
ls -la "$OUT_DIR/wpa_supplicant" "$OUT_DIR/wpa_cli"
