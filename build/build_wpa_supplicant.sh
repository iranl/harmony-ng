#!/bin/bash
# Build wpa_supplicant and wpa_cli for the Linux 6.18 Hub system (nl80211 driver, WPA2-PSK and
# WPA3-Personal/SAE) as MIPS binaries on OpenWrt's libnl-tiny and wolfSSL. Run in WSL/Linux.
# Output: $OUT_DIR/wpa_supplicant, $OUT_DIR/wpa_cli (default build/output/kernel6/bin); on the
# Hub they go to /mnt/data/kernel6/bin (build/kernel6/rc.sh).
#
# Crypto: wolfSSL (GPLv2+), linked statically into wpa_supplicant only, for the elliptic curves of
# SAE that the internal crypto lacks. Its release zip carries a ready configure script (no
# autotools needed). wpa_supplicant uses no TLS here (no EAP), and --gc-sections drops what SAE
# and WPA2 do not call: about 170 KB more than the internal crypto.
set -euo pipefail

WPA_VER=2.11
WPA_SHA256=912ea06f74e30a8e36fbb68064d6cdff218d8d591db0fc5d75dee6c81ac7fc0a
# libnl-tiny has no releases: a fixed commit of github.com/openwrt/libnl-tiny
NL_COMMIT="${NL_COMMIT:-40493a655d8caa2ccf5206dde1e733abe2920432}"
WOLF_VER=5.7.6
WOLF_SHA256=cbb9da98a678156b1b294301ffd0139cfae19c60b46dff397418940690291a20
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TC="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
WORK="${WORK:-$HOME/kbuild6/wpa}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build/output/kernel6/bin}"
# against the toolchain's shared uClibc-ng, for the Linux 6.18 root file system
STATIC=""

[ -x "${TC}gcc" ] || { echo "Error: ${TC}gcc not found" >&2; exit 1; }
mkdir -p "$WORK" "$OUT_DIR"
cd "$WORK"
[ -f "wpa_supplicant-$WPA_VER.tar.gz" ] || curl -fsSLO "https://w1.fi/releases/wpa_supplicant-$WPA_VER.tar.gz"
echo "$WPA_SHA256  wpa_supplicant-$WPA_VER.tar.gz" | sha256sum -c --quiet -
curl -fsSL -o libnl-tiny.tar.gz "https://github.com/openwrt/libnl-tiny/archive/$NL_COMMIT.tar.gz"
[ -f "wolfssl-$WOLF_VER.zip" ] || curl -fsSLO "https://www.wolfssl.com/wolfssl-$WOLF_VER.zip"
echo "$WOLF_SHA256  wolfssl-$WOLF_VER.zip" | sha256sum -c --quiet -

echo "=== libnl-tiny ==="
rm -rf libnl-tiny-* nl && mkdir nl
tar -xzf libnl-tiny.tar.gz
(cd libnl-tiny-*/ && for f in *.c; do
    "${TC}gcc" -Os -D_GNU_SOURCE -Iinclude -c "$f" -o "$WORK/nl/${f%.c}.o"
done && cp -r include "$WORK/nl/include")
"${TC}ar" rcs nl/libnl-tiny.a nl/*.o

echo "=== wolfSSL $WOLF_VER ==="
rm -rf "wolfssl-$WOLF_VER" wolf
unzip -q "wolfssl-$WOLF_VER.zip"
(cd "wolfssl-$WOLF_VER" && \
    CFLAGS="-Os -ffunction-sections -fdata-sections" ./configure --host=mips-buildroot-linux-uclibc \
        CC="${TC}gcc" AR="${TC}ar" RANLIB="${TC}ranlib" --prefix="$WORK/wolf" \
        --enable-static --disable-shared --enable-wpas --disable-examples --disable-crypttests \
        --disable-benchmark > configure.log 2>&1 || { tail -n 20 configure.log >&2; exit 1; }
    make -j"$(nproc)" > make.log 2>&1 || { grep -n -m 20 -i "error" make.log >&2; exit 1; }
    make install > /dev/null)

echo "=== wpa_supplicant $WPA_VER ==="
rm -rf "wpa_supplicant-$WPA_VER"
tar -xzf "wpa_supplicant-$WPA_VER.tar.gz"
cd "wpa_supplicant-$WPA_VER/wpa_supplicant"
cat > .config <<EOF
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL_TINY=y
CONFIG_CTRL_IFACE=y
# -s (log to syslog): HarmonyNG's network_manager.sh starts it so, as on the stock firmware
CONFIG_DEBUG_SYSLOG=y
CONFIG_BACKEND=file
CONFIG_TLS=wolfssl
CONFIG_SAE=y
CONFIG_NO_CONFIG_BLOBS=y
CONFIG_NO_RANDOM_POOL=y
CONFIG_ELOOP_POLL=y
CONFIG_SMALL_PKG=y
EOF
make -j"$(nproc)" CC="${TC}gcc" \
    EXTRA_CFLAGS="-Os -ffunction-sections -fdata-sections -D_GNU_SOURCE -I$WORK/nl/include -I$WORK/wolf/include" \
    LDFLAGS="$STATIC -Wl,--gc-sections -L$WORK/nl -L$WORK/wolf/lib" wpa_supplicant wpa_cli > build.log 2>&1 \
    || { grep -n -m 20 -i "error\|undefined" build.log >&2; exit 1; }
"${TC}strip" wpa_supplicant wpa_cli
cp wpa_supplicant wpa_cli "$OUT_DIR/"
ls -la "$OUT_DIR/wpa_supplicant" "$OUT_DIR/wpa_cli"
