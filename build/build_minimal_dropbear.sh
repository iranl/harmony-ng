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
BUILD_DIR="$DB_DIR/dropbear-2024.86"

cd "$DB_DIR"
if [ ! -d "$BUILD_DIR" ]; then
    tar -xjf dropbear-2024.86.tar.bz2
fi

cd "$BUILD_DIR"

# Without password auth dropbear rejects -s and -g, which every start script passes (older
# binaries need them). Accept them as no-ops here: there is no password login to refuse.
if ! grep -q 'no password login to refuse' src/svr-runopts.c; then
    sed -i "/^[[:space:]]*case 'h':/i\\
#if !(DROPBEAR_SVR_PASSWORD_AUTH || DROPBEAR_SVR_PAM_AUTH)\\
\t\t\t\tcase 's':\\
\t\t\t\tcase 'g':\\
\t\t\t\t\t/* no password login to refuse */\\
\t\t\t\t\tbreak;\\
#endif" src/svr-runopts.c
fi

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
# -mno-abicalls -fno-pic: static code needs no GOT loads or call stubs (ld adds stubs into libc)
CFLAGS="-D_FORTIFY_SOURCE=2 -fno-strict-overflow -Wno-pointer-sign -Os -flto -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -mno-abicalls -fno-pic"
LDFLAGS="-Os -flto -static -Wl,--gc-sections -Wl,-z,norelro -Wl,--build-id=none -Wl,--hash-style=sysv"

./configure --host=mips-buildroot-linux-uclibc \
    --prefix=/ \
    --disable-zlib \
    --disable-syslog \
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
