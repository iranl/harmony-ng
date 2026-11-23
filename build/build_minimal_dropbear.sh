#!/bin/bash
# Minimal Hub dropbearmulti (dropbear dropbearkey scp). Hub SSH takes Ed25519 keys only and
# OpenSSH clients only: curve25519 key exchange, chacha20-poly1305 (its own MAC, so no HMAC),
# no RSA/DSS/ECDSA/DH (so no bignum code), no passwords, no forwarding.
# Static: the toolchain (uClibc-ng) and the stock uClibc 0.9.30 differ in ABI (sigset_t is 16 vs
# 128 bytes), so a dynamic build against the Hub's /lib overruns every sigset_t and segfaults.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DB_DIR="$SCRIPT_DIR/tmp/dropbear"
# https://matt.ucc.asn.au/dropbear/releases/ (SHA-256 from its SHA256SUM.asc)
DB_VERSION="2026.94"
DB_SHA256="e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81"
BUILD_DIR="$DB_DIR/dropbear-$DB_VERSION"

cd "$DB_DIR"
# the tarball is checked on every build, not only the first unpack
echo "$DB_SHA256  dropbear-$DB_VERSION.tar.bz2" | sha256sum -c -
if [ ! -d "$BUILD_DIR" ]; then
    tar -xjf "dropbear-$DB_VERSION.tar.bz2"
fi

cd "$BUILD_DIR"

for t in "$REPO_ROOT/build/toolchains"/mips32--uclibc--stable*; do
    if [ -d "$t/bin" ]; then
        export PATH="$t/bin:$PATH"
        break
    fi
done

cat << 'EOF' > localoptions.h
#define DROPBEAR_DEFPORT "22"
#define INETD_MODE 0
#define DROPBEAR_RSA 0
#define DROPBEAR_DSS 0
#define DROPBEAR_ECDSA 0
#define DROPBEAR_ED25519 1
#define DROPBEAR_SK_KEYS 0
#define DROPBEAR_DH_GROUP14_SHA1 0
#define DROPBEAR_DH_GROUP14_SHA256 0
#define DROPBEAR_DH_GROUP16 0
#define DROPBEAR_ECDH 0
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_AES128 0
#define DROPBEAR_AES256 0
#define DROPBEAR_3DES 0
#define DROPBEAR_CHACHA20POLY1305 1
#define DROPBEAR_ENABLE_CTR_MODE 0
#define DROPBEAR_ENABLE_CBC_MODE 0
#define DROPBEAR_ENABLE_GCM_MODE 0
#define DROPBEAR_SHA1_HMAC 0
#define DROPBEAR_SHA2_256_HMAC 0
#define DROPBEAR_SHA2_512_HMAC 0
#define DROPBEAR_SHA1_96_HMAC 0
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_USER_ALGO_LIST 0
#define DROPBEAR_SFTPSERVER 0
#define DO_MOTD 0
#define DO_HOST_LOOKUP 0
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
EOF

CC="mips-buildroot-linux-uclibc-gcc"
STRIP="mips-buildroot-linux-uclibc-strip"
# -flto objects need the plugin-aware archiver
export AR="mips-buildroot-linux-uclibc-gcc-ar" RANLIB="mips-buildroot-linux-uclibc-gcc-ranlib"

# --disable-harden below drops PIE and relro (size); its other flags are kept here.
# No -mno-abicalls: mixed with the PIC static libc, scp -t segfaults on the Hub (-13 KB only).
CFLAGS="-D_FORTIFY_SOURCE=2 -fno-strict-overflow -Wno-pointer-sign -Os -flto -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections"
LDFLAGS="-Os -flto -static -Wl,--gc-sections -Wl,-z,norelro -Wl,--build-id=none -Wl,--hash-style=sysv"

# syslog stays on (about 3 KB): the Hub runs syslogd, and a refused key or a missing host key
# would otherwise leave no trace there
./configure --host=mips-buildroot-linux-uclibc \
    --prefix=/ \
    --disable-zlib \
    --disable-lastlog \
    --disable-utmp \
    --disable-utmpx \
    --disable-wtmp \
    --disable-wtmpx \
    --disable-loginfunc \
    --disable-harden \
    --enable-bundled-libtom \
    CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"

make clean || true
make -j"$(nproc)" PROGRAMS="dropbear dropbearkey scp" MULTI=1
"$STRIP" -s -R .comment -R .note -R .note.ABI-tag -R .gnu.attributes dropbearmulti

echo "=== RESULT DROPBEARMULTI ==="
ls -l dropbearmulti
mips-buildroot-linux-uclibc-size dropbearmulti
