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

# Sync version across components before compilation
PYTHON=""
if command -v python3 >/dev/null 2>&1; then
    PYTHON="python3"
elif command -v python >/dev/null 2>&1; then
    PYTHON="python"
fi
if [ -n "$PYTHON" ] && [ -f "$REPO_ROOT/scripts/bump_version.py" ]; then
    echo "[-] Syncing project version from VERSION..."
    "$PYTHON" "$REPO_ROOT/scripts/bump_version.py" --sync
fi

CFLAGS="-Wall -Os -static -s -DNDEBUG"
COMPAT_SRC=""

TARGET="${1:-all}"

build_util() {
    echo "[-] Building hub_util..."
    "$CC" $CFLAGS -Ipayload/source -o "$BIN/hub_util" payload/source/hub_util.c payload/source/hub_ntp.c $COMPAT_SRC
    for app in hub_sync sync mknod register_ehci eth_fix hub_dhcpd hub_portal hub_sntp; do
        ln -sf hub_util "$BIN/$app"
    done
}

build_dhcpd() {
    build_util
}

build_portal() {
    build_util
}

build_sntp() {
    build_util
}

build_daemon() {
    echo "[-] Building hub_daemon..."
    "$CC" $CFLAGS -Ipayload/source -o "$BIN/hub_daemon" \
        payload/source/hub_daemon.c \
        payload/source/http_server.c \
        payload/source/http_util.c \
        payload/source/http_auth.c \
        payload/source/http_api_auth.c \
        payload/source/http_api_activities.c \
        payload/source/http_api_devices.c \
        payload/source/http_api_commands.c \
        payload/source/http_api_custom_icons.c \
        payload/source/http_api_network.c \
        payload/source/hub_network_util.c \
        payload/source/http_api_remotes.c \
        payload/source/http_api_ota.c \
        payload/source/http_api_backup.c \
        payload/source/http_api_system.c \
        payload/source/config_store.c \
        payload/source/bt_migrate.c \
        payload/source/config_sanitize.c \
        payload/source/orchestrator.c \
        payload/source/orchestrator_queue.c \
        payload/source/hub_mqtt.c \
        payload/source/hub_mqtt_ha.c \
        payload/source/hub_ws.c \
        payload/source/hub_ntp.c \
        payload/source/resource_cache.c \
        payload/source/cJSON.c \
        payload/source/ir_encoder.c \
        payload/source/ir_i2s.c \
        payload/source/hw_action.c \
        payload/source/ws_server.c \
        payload/source/state_watch.c \
        payload/source/hub_events.c \
        payload/source/hub_info.c \
        payload/source/hub_mdns.c \
        payload/source/hub_cmd.c \
        payload/source/mfg_id.c \
        payload/source/mqtt_client.c \
        payload/source/sha256.c \
        payload/source/timezone_db.c \
        payload/source/uclibc_static_locks.c \
        $COMPAT_SRC -lpthread -lm
}

build_register_ehci() {
    build_util
}

build_eth_fix() {
    build_util
}

build_mknod() {
    build_util
}


build_btstack() {
    echo "[-] Building hub_btstack..."
    sh "$SCRIPT_DIR/build_btstack.sh"
}

build_sync() {
    build_util
}

build_rf() {
    echo "[-] Building hub_rf..."
    RF_SRCS="payload/source/rf_state.c payload/source/rf_spi.c payload/source/rf_keymap.c payload/source/rf_ota.c payload/source/rf_sync.c payload/source/rf_pairing.c payload/source/rf_config_push.c payload/source/rf_push_queue.c payload/source/rf_help.c payload/source/rf_api.c payload/source/rf_main.c payload/source/remote_stock_icons.c payload/source/custom_icon_registry.c payload/source/timezone_db.c payload/source/sha256.c"
    "$CC" $CFLAGS -Ipayload/source -o "$BIN/hub_rf" $RF_SRCS payload/source/cJSON.c payload/source/uclibc_static_locks.c $COMPAT_SRC -lpthread -lm
}

build_remote() {
    echo "[-] Building remote_ui (ARM)..."
    ARM_CC=""
    for t in "$SCRIPT_DIR/toolchains"/armv5-eabi--uclibc--stable*; do
        if [ -d "$t/bin" ]; then
            export PATH="$t/bin:$PATH"
            break
        fi
    done
    if command -v arm-buildroot-linux-uclibcgnueabi-gcc >/dev/null 2>&1; then
        ARM_CC="arm-buildroot-linux-uclibcgnueabi-gcc"
    elif command -v arm-linux-gnueabi-gcc >/dev/null 2>&1; then
        ARM_CC="arm-linux-gnueabi-gcc"
    elif command -v arm-linux-gcc >/dev/null 2>&1; then
        ARM_CC="arm-linux-gcc"
    else
        echo "[!] No ARM cross-compiler found. Extract Bootlin ARM toolchain into build/toolchains/." >&2
        exit 1
    fi

    mkdir -p "$REPO_ROOT/remote/bin" "$REPO_ROOT/build/output/remote"
    echo "[-] Building remote_ui (ARM)..."
    "$ARM_CC" -static -Os -s -Wall -Ipayload/source -Ipayload/source/remote payload/source/remote/*.c payload/source/remote_stock_icons.c payload/source/mfg_id.c payload/source/timezone_db.c payload/source/cJSON.c payload/source/sha256.c payload/source/uclibc_static_locks.c -o "$REPO_ROOT/remote/bin/remote_ui" -lpthread -lm
    echo "[-] Building remote_sync (ARM)..."
    "$ARM_CC" -static -Os -s -Wall payload/source/hub_sync.c -o "$REPO_ROOT/remote/bin/remote_sync"
    echo "[-] Packaging remote_ui.ota.gz (binary + scripts)..."
    mkdir -p "$REPO_ROOT/build/tmp/remote_ota_pkg"
    cp -f "$REPO_ROOT/remote/bin/remote_ui" "$REPO_ROOT/remote/bin/remote_sync" "$REPO_ROOT/build/tmp/remote_ota_pkg/"
    # Transition: a remote still on a build from before the elite -> remote rename looks for
    # "elite_ui" in the package and installs it as /data/elite_ui; init.sh migrate (run by that
    # binary at start) then moves it to /data/remote_ui. Drop this copy once no such remote is left.
    cp -f "$REPO_ROOT/remote/bin/remote_ui" "$REPO_ROOT/build/tmp/remote_ota_pkg/elite_ui"
    cp -f "$REPO_ROOT/remote/rcS.local" "$REPO_ROOT/remote/init.sh" "$REPO_ROOT/remote/remote_watchdog.sh" "$REPO_ROOT/remote/factory_reset.sh" "$REPO_ROOT/remote/safe_reboot.sh" "$REPO_ROOT/build/tmp/remote_ota_pkg/" 2>/dev/null || true
    sed -i 's/\r$//' "$REPO_ROOT/build/tmp/remote_ota_pkg"/*.sh "$REPO_ROOT/build/tmp/remote_ota_pkg"/rcS.local 2>/dev/null || true
    # Theme packages. Building them needs a host C compiler and the
    # pinned downloads; without them, themes built earlier into build/tmp/remote-themes are used.
    echo "[-] Building remote themes..."
    python3 "$REPO_ROOT/tools/remote_render/build_theme.py" >/dev/null \
        || echo "[!] Theme build failed; packaging themes from an earlier build, if any." >&2
    rm -rf "$REPO_ROOT/remote/bin/themes"
    if [ -d "$REPO_ROOT/build/tmp/remote-themes" ]; then
        cp -R "$REPO_ROOT/build/tmp/remote-themes" "$REPO_ROOT/remote/bin/themes"
        cp -R "$REPO_ROOT/build/tmp/remote-themes" "$REPO_ROOT/build/tmp/remote_ota_pkg/themes"
    fi
    tar -czf "$REPO_ROOT/remote/bin/remote_ui.ota.gz" -C "$REPO_ROOT/build/tmp/remote_ota_pkg" .
    cp -f "$REPO_ROOT/remote/bin/remote_ui.ota.gz" "$REPO_ROOT/remote/bin/remote_ui" "$REPO_ROOT/remote/bin/remote_sync" "$REPO_ROOT/build/output/remote/" 2>/dev/null || true
    rm -rf "$REPO_ROOT/build/output/remote/themes"
    [ -d "$REPO_ROOT/remote/bin/themes" ] && cp -R "$REPO_ROOT/remote/bin/themes" "$REPO_ROOT/build/output/remote/themes"
    rm -rf "$REPO_ROOT/build/tmp/remote_ota_pkg"
}


case "$TARGET" in
    remote|elite)
        build_remote
        exit 0
        ;;
    daemon)
        build_daemon
        ;;
    util|hub_util)
        build_util
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
        build_util
        build_daemon
        build_rf
        ;;
    all)
        build_util
        build_daemon
        build_rf
        build_btstack
        ;;
    *)
        echo "Unknown target: $TARGET"
        echo "Usage: $0 [all|hub|daemon|util|sntp|portal|dhcpd|btstack|register_ehci|eth_fix|mknod|rf|remote|apk] (elite = remote)"
        exit 1
        ;;
esac

echo "Copying binaries to build/output..."
if [ -f "$REPO_ROOT/tools/bin/dropbearmulti_mips" ] && [ ! -f "$BIN/dropbearmulti" ]; then
    cp -f "$REPO_ROOT/tools/bin/dropbearmulti_mips" "$BIN/dropbearmulti"
fi
cp -a "$BIN"/hub_* "$BIN"/register_ehci "$BIN"/eth_fix "$BIN"/mknod "$BIN"/sync "$BIN"/dropbearmulti "$OUT"/ 2>/dev/null || cp -f "$BIN"/hub_* "$BIN"/register_ehci "$BIN"/eth_fix "$BIN"/mknod "$BIN"/sync "$BIN"/dropbearmulti "$OUT"/ 2>/dev/null || true

echo "Updating MANIFEST.txt and FILES..."
cd "$BIN"
file hub_util hub_btstack hub_dhcpd hub_portal hub_daemon hub_sntp hub_rf dropbearmulti register_ehci mknod > FILES 2>/dev/null || true
ls -l hub_util hub_btstack hub_dhcpd hub_portal hub_daemon hub_sntp hub_rf dropbearmulti register_ehci mknod | sed -E 's/ [a-zA-Z0-9_-]+ [a-zA-Z0-9_-]+ / root root /' > MANIFEST.txt 2>/dev/null || true
md5sum hub_util hub_btstack hub_dhcpd hub_portal hub_daemon hub_sntp hub_rf dropbearmulti register_ehci mknod >> MANIFEST.txt 2>/dev/null || true

echo "=== Build finished successfully! ==="
ls -lh "$BIN"/hub_* "$BIN"/register_ehci "$BIN"/mknod 2>/dev/null || true
