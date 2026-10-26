#!/bin/bash
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

for t in "$REPO_ROOT/build/toolchains"/mips32--uclibc--stable*; do
    if [ -d "$t/bin" ]; then
        export PATH="$t/bin:$PATH"
        break
    fi
done

cat << 'EOF' > localoptions.h
#define DROPBEAR_DEFPORT "22"
#define DROPBEAR_RSA 1
#define DROPBEAR_ED25519 1
#define DROPBEAR_ECDSA 0
#define DROPBEAR_DSS 0
#define DROPBEAR_AES128 1
#define DROPBEAR_AES256 0
#define DROPBEAR_3DES 0
#define DROPBEAR_TWOFISH256 0
#define DROPBEAR_TWOFISH128 0
#define DROPBEAR_BLOWFISH 0
#define DROPBEAR_CHACHA20POLY1305 1
#define DROPBEAR_SHA1 0
#define DROPBEAR_SHA2_256 1
#define DROPBEAR_SHA2_512 0
#define DROPBEAR_X11FWD 0
#define DROPBEAR_AGENTFWD 0
#define DO_MOTD 0
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PUBKEY_AUTH 1
EOF

CC="mips-buildroot-linux-uclibc-gcc"
STRIP="mips-buildroot-linux-uclibc-strip"

CFLAGS="-Os -s -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections"
LDFLAGS="-Wl,--gc-sections -static"

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
    --enable-bundled-libtom \
    CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"

make clean || true
make -j"$(nproc)" PROGRAMS="dropbear dropbearkey scp" MULTI=1
"$STRIP" dropbearmulti

echo "=== RESULT DROPBEARMULTI ==="
ls -lh dropbearmulti
mips-buildroot-linux-uclibc-size dropbearmulti
