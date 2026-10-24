#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

SRC="$REPO_ROOT/payload/source"
BIN="$REPO_ROOT/payload/bin"
OUT="$SCRIPT_DIR/output"
for t in "$SCRIPT_DIR/toolchains"/mips32--uclibc--stable*; do
    if [ -d "$t/bin" ]; then
        export PATH="$t/bin:$PATH"
        break
    fi
done

if [ -z "${CC:-}" ]; then
    if command -v mips-buildroot-linux-uclibc-gcc >/dev/null 2>&1; then
        CC="mips-buildroot-linux-uclibc-gcc"
        STRIP="mips-buildroot-linux-uclibc-strip"
    elif command -v mips-linux-gnu-gcc >/dev/null 2>&1; then
        CC="mips-linux-gnu-gcc"
        STRIP="mips-linux-gnu-strip"
    else
        CC="mips-buildroot-linux-uclibc-gcc"
        STRIP="mips-buildroot-linux-uclibc-strip"
    fi
fi
if [ -z "${STRIP:-}" ]; then
    STRIP="strip"
fi

mkdir -p "$BIN" "$OUT"

cd "$REPO_ROOT"
CFLAGS="-Os -static -s -DNDEBUG"
COMPAT_SRC=""

TARGET="${1:-all}"

build_dhcpd() {
    echo "[-] Building codex_dhcpd..."
    "$CC" $CFLAGS -o "$BIN/codex_dhcpd" payload/source/hub_dhcpd.c $COMPAT_SRC
}

build_portal() {
    echo "[-] Building codex_portal..."
    "$CC" $CFLAGS -o "$BIN/codex_portal" payload/source/hub_portal.c $COMPAT_SRC
}

build_sntp() {
    echo "[-] Building codex_sntp..."
    "$CC" $CFLAGS -Ipayload/source -o "$BIN/codex_sntp" payload/source/codex_sntp_main.c payload/source/codex_ntp.c $COMPAT_SRC
}

build_daemon() {
    echo "[-] Building codex_daemon..."
    "$CC" $CFLAGS -Ipayload/source -o "$BIN/codex_daemon" \
        payload/source/codex_daemon.c \
        payload/source/config_store.c \
        payload/source/resource_cache.c \
        payload/source/cJSON.c \
        payload/source/ir_encoder.c \
        payload/source/ir_i2s.c \
        payload/source/hw_action.c \
        payload/source/orchestrator.c \
        payload/source/ws_server.c \
        payload/source/http_server.c \
        payload/source/mqtt_client.c \
        payload/source/codex_ntp.c \
        $COMPAT_SRC -lpthread -lm
}

build_register_ehci() {
    echo "[-] Building register_ehci..."
    "$CC" $CFLAGS -o "$BIN/register_ehci" payload/source/register_ehci.c $COMPAT_SRC
}

build_eth_fix() {
    echo "[-] Building eth_fix..."
    "$CC" $CFLAGS -o "$BIN/eth_fix" payload/source/eth_fix.c $COMPAT_SRC
}

build_mknod() {
    echo "[-] Building mknod..."
    "$CC" $CFLAGS -o "$BIN/mknod" payload/source/mknod.c $COMPAT_SRC
}


build_btstack() {
    echo "[-] Building codex_btstack..."
    "$SCRIPT_DIR/build_btstack.sh"
}

build_sync() {
    echo "[-] Building codex_sync..."
    "$CC" $CFLAGS -o "$BIN/codex_sync" payload/source/codex_sync.c $COMPAT_SRC
}

build_rf() {
    echo "[-] Building codex_rf..."
    "$CC" $CFLAGS -Ipayload/source -o "$BIN/codex_rf" payload/source/codex_rf.c payload/source/cJSON.c $COMPAT_SRC -lpthread -lm
}

build_elite() {
    echo "[-] Building codex_elite (ARM)..."
    mkdir -p "$REPO_ROOT/elite/bin" "$REPO_ROOT/build/output/elite"
    arm-linux-gnueabi-gcc -static -Os -s -Wall -Ipayload/source payload/source/codex_elite.c payload/source/cJSON.c -o "$REPO_ROOT/elite/bin/codex_elite" -lpthread -lm
    echo "[-] Building codex_sync (ARM)..."
    arm-linux-gnueabi-gcc -static -Os -s -o "$REPO_ROOT/elite/bin/codex_sync" payload/source/codex_sync.c
    echo "[-] Packaging codex_elite.ota.gz (binary + scripts)..."
    mkdir -p "$REPO_ROOT/build/tmp/elite_ota_pkg"
    cp -f "$REPO_ROOT/elite/bin/codex_elite" "$REPO_ROOT/build/tmp/elite_ota_pkg/"
    cp -f "$REPO_ROOT/elite/rcS.local" "$REPO_ROOT/elite/codex_watchdog.sh" "$REPO_ROOT/elite/factory_reset.sh" "$REPO_ROOT/build/tmp/elite_ota_pkg/" 2>/dev/null || true
    tar -czf "$REPO_ROOT/elite/bin/codex_elite.ota.gz" -C "$REPO_ROOT/build/tmp/elite_ota_pkg" .
    cp -f "$REPO_ROOT/elite/bin/codex_elite.ota.gz" "$REPO_ROOT/build/output/elite/" 2>/dev/null || true
    rm -rf "$REPO_ROOT/build/tmp/elite_ota_pkg"
}


case "$TARGET" in
    elite)
        build_elite
        exit 0
        ;;
    daemon)
        build_daemon
        ;;
    sntp)
        build_sntp
        ;;
    portal)
        build_portal
        ;;
    dhcpd)
        build_dhcpd
        ;;
    btstack)
        build_btstack
        ;;
    register_ehci)
        build_register_ehci
        ;;
    mknod)
        build_mknod
        ;;
    sync)
        build_sync
        ;;
    rf)
        build_rf
        ;;
    apk)
        echo "[-] Building Android APK..."
        cd "$REPO_ROOT/android-app"
        npm run build
        npx cap sync android
        cd android && ./gradlew assembleDebug
        mkdir -p "$OUT"
        cp -f app/build/outputs/apk/debug/app-debug.apk "$OUT/app-debug.apk"
        echo "[+] APK copied to $OUT/app-debug.apk"
        ;;
    eth_fix)
        build_eth_fix
        ;;
    hub)
        build_dhcpd
        build_portal
        build_sntp
        build_daemon
        build_register_ehci
        build_eth_fix
        build_mknod
        build_sync
        build_rf
        ;;
    all)
        build_dhcpd
        build_portal
        build_sntp
        build_daemon
        build_register_ehci
        build_eth_fix
        build_mknod
        build_sync
        build_rf
        build_btstack
        ;;
    *)
        echo "Unknown target: $TARGET"
        echo "Usage: $0 [all|hub|daemon|sntp|portal|dhcpd|btstack|register_ehci|eth_fix|mknod|rf|elite|apk]"
        exit 1
        ;;
esac

echo "Copying binaries to build/output..."
cp -f "$BIN"/codex_* "$BIN"/register_ehci "$BIN"/eth_fix "$BIN"/mknod "$OUT"/ 2>/dev/null || true

echo "Updating MANIFEST.txt and FILES..."
cd "$BIN"
file codex_btstack codex_dhcpd codex_portal codex_daemon codex_sntp codex_rf dropbearmulti register_ehci mknod > FILES 2>/dev/null || true
ls -l codex_btstack codex_dhcpd codex_portal codex_daemon codex_sntp codex_rf dropbearmulti register_ehci mknod | sed -E 's/ [a-zA-Z0-9_-]+ [a-zA-Z0-9_-]+ / root root /' > MANIFEST.txt 2>/dev/null || true
md5sum codex_btstack codex_dhcpd codex_portal codex_daemon codex_sntp codex_rf dropbearmulti register_ehci mknod >> MANIFEST.txt 2>/dev/null || true

echo "=== Build finished successfully! ==="
ls -lh "$BIN"/codex_* "$BIN"/register_ehci "$BIN"/mknod 2>/dev/null || true
