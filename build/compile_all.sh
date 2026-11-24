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

# Section GC: each function and object in its own section, the unreferenced ones dropped at link
CFLAGS="-Wall -Os -flto -static -s -DNDEBUG -ffunction-sections -fdata-sections -Wl,--gc-sections"
# The commit the Hub reports (/api/status and the OTA status said "dev" for every release)
GIT_COMMIT=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)
CFLAGS="$CFLAGS -DHARMONY_COMMIT=\"$GIT_COMMIT\""
# Binaries newer than this stamp were built by this run (see "Copying binaries" below)
BUILD_STAMP="$OUT/.build_stamp"
: > "$BUILD_STAMP"
# Extra OTA release key for test builds only, e.g. OTA_KEY_CFLAGS=-DOTA_EXTRA_PUBKEY=0x12,0x34,... (32 bytes)
OTA_KEY_CFLAGS="${OTA_KEY_CFLAGS:-}"
COMPAT_SRC=""
# Build date in UTC (YYYYMMDD) for the time sync in hub_ntp.c: a clock before it is not set yet
CFLAGS="$CFLAGS -DHUB_BUILD_DATE_UTC=$(date -u +%Y%m%d)"

TARGET="${1:-all}"

# BearSSL (build/fetch_bearssl.sh pins the commit) is on the Hub once: payload/lib/libbearssl.so.0,
# trimmed to the functions in build/bearssl_exports.txt. hub_fetch (the Hub's only TLS client) and
# the plugins link it (-l:libbearssl.so.0, started with LD_LIBRARY_PATH=/data/harmony/lib: the
# stock ld-uClibc ignores rpath). The static core binaries carry no TLS (hub_daemon runs hub_fetch).
BEARSSL_DIR="${BEARSSL_DIR:-$SCRIPT_DIR/tmp/bearssl}"
BEARSSL_OUT="$SCRIPT_DIR/tmp/bearssl_mips"
BEARSSL_SONAME="libbearssl.so.0"
BEARSSL_EXPORTS="$SCRIPT_DIR/bearssl_exports.txt"
LIB="$REPO_ROOT/payload/lib"
READELF="${CC%gcc}readelf"

# fails when a dynamic binary or library imports what the Hub's stock uClibc 0.9.30 lacks
check_stock_libc() {
    if "$READELF" --dyn-syms -W "$1" | grep -q '__ctype_'; then
        echo "[!] $1 imports __ctype_* symbols the Hub's uClibc lacks" >&2
        exit 1
    fi
    needed=$("$READELF" -d "$1" | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/' | tr '\n' ' ')
    if [ "$needed" != "$2" ]; then
        echo "[!] $1 needs '$needed', not '$2'" >&2
        exit 1
    fi
}

build_bearssl() {
    BEARSSL_DIR="$BEARSSL_DIR" sh "$SCRIPT_DIR/fetch_bearssl.sh"
    stamp="$(git -C "$BEARSSL_DIR" rev-parse HEAD) $CC $BEARSSL_SONAME $(md5sum < "$BEARSSL_EXPORTS" | cut -c1-32) 2"
    if [ -f "$LIB/$BEARSSL_SONAME" ] && [ "$(cat "$BEARSSL_OUT/stamp" 2>/dev/null)" = "$stamp" ]; then
        return 0
    fi
    echo "[-] Building BearSSL..."
    mkdir -p "$BEARSSL_OUT/pic" "$LIB"
    # the objects only depend on the commit and compiler (one gcc call: far faster than one per file)
    if [ "$(cat "$BEARSSL_OUT/pic/stamp" 2>/dev/null)" != "$(git -C "$BEARSSL_DIR" rev-parse HEAD) $CC" ]; then
        rm -f "$BEARSSL_OUT"/pic/*
        C_FILES=$(cd "$BEARSSL_DIR" && find src -name '*.c' ! -name 'brssl.c' ! -name 'test_*.c' | sort | sed "s|^|$BEARSSL_DIR/|")
        (cd "$BEARSSL_OUT/pic" && "$CC" -Os -Wall -fPIC -ffunction-sections -fdata-sections \
            -I"$BEARSSL_DIR/inc" -I"$BEARSSL_DIR/src" -c $C_FILES)
        echo "$(git -C "$BEARSSL_DIR" rev-parse HEAD) $CC" > "$BEARSSL_OUT/pic/stamp"
    fi
    # only the listed names are exported; what they do not reach is dropped
    { echo "{ global:"; grep -v '^#' "$BEARSSL_EXPORTS" | sed '/^$/d; s/$/;/'; echo "local: *; };"; } > "$BEARSSL_OUT/exports.map"
    "$CC" -shared -s -Wl,--gc-sections -Wl,--version-script="$BEARSSL_OUT/exports.map" \
        -Wl,-soname,"$BEARSSL_SONAME" -o "$LIB/$BEARSSL_SONAME" "$BEARSSL_OUT"/pic/*.o
    check_stock_libc "$LIB/$BEARSSL_SONAME" "libc.so.0 "
    echo "$stamp" > "$BEARSSL_OUT/stamp"
    echo "[+] $BEARSSL_SONAME: $(wc -c < "$LIB/$BEARSSL_SONAME") bytes"
}

# hub_fetch: dynamic (stock libc + libbearssl.so.0), not -static like the core binaries
build_fetch() {
    build_bearssl
    echo "[-] Building hub_fetch..."
    "$CC" -Wall -Os -s -DNDEBUG -DHUB_BUILD_DATE_UTC=$(date -u +%Y%m%d) -ffunction-sections -fdata-sections \
        -Wl,--gc-sections -Ipayload/source -I"$BEARSSL_DIR/inc" -o "$BIN/hub_fetch" \
        payload/source/hub_fetch.c payload/source/tls_client.c payload/source/http_client.c \
        -L"$LIB" -l:"$BEARSSL_SONAME"
    check_stock_libc "$BIN/hub_fetch" "$BEARSSL_SONAME libc.so.0 "
}

build_util() {
    echo "[-] Building hub_util..."
    # unused functions are left out (the Wi-Fi code shares http_util.c with hub_daemon)
    "$CC" $CFLAGS $OTA_KEY_CFLAGS -Ipayload/source -o "$BIN/hub_util" payload/source/hub_util.c payload/source/hub_ntp.c payload/source/ota_verify.c payload/source/tweetnacl.c payload/source/sha256.c payload/source/cJSON.c payload/source/usb_wifi.c payload/source/wifi_config.c payload/source/http_util.c $COMPAT_SRC -lm
    for app in hub_sync sync mknod register_ehci eth_fix hub_sntp hub_usbwifi; do
        ln -sf hub_util "$BIN/$app"
    done
}

build_sntp() {
    build_util
}

build_daemon() {
    echo "[-] Building hub_daemon..."
    # -DNET_NO_TLS: plain HTTP/TCP in process, TLS through hub_fetch (no BearSSL in here)
    "$CC" $CFLAGS -DNET_NO_TLS -Ipayload/source -o "$BIN/hub_daemon" \
        payload/source/hub_daemon.c \
        payload/source/http_server.c \
        payload/source/http_util.c \
        payload/source/http_auth.c \
        payload/source/http_api_auth.c \
        payload/source/http_api_activities.c \
        payload/source/http_api_devices.c \
        payload/source/http_api_commands.c \
        payload/source/http_api_custom_icons.c \
        payload/source/icon_pack.c \
        payload/source/http_api_network.c \
        payload/source/hub_network_util.c \
        payload/source/wifi_config.c \
        payload/source/http_api_remotes.c \
        payload/source/http_api_ota.c \
        payload/source/http_api_backup.c \
        payload/source/http_api_system.c \
        payload/source/http_api_plugins.c \
        payload/source/plugin_mgr.c \
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
        payload/source/hub_ssdp.c \
        payload/source/hbus_config.c \
        payload/source/hbus_server.c \
        payload/source/hub_cmd.c \
        payload/source/mfg_id.c \
        payload/source/mqtt_client.c \
        payload/source/sha256.c \
        payload/source/timezone_db.c \
        payload/source/uclibc_static_locks.c \
        payload/source/tls_client.c \
        payload/source/http_client.c \
        payload/source/net_action.c \
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
    RF_SRCS="payload/source/rf_state.c payload/source/rf_spi.c payload/source/rf_keymap.c payload/source/rf_ota.c payload/source/rf_sync.c payload/source/rf_pairing.c payload/source/rf_config_push.c payload/source/rf_button_list.c payload/source/rf_push_queue.c payload/source/rf_help.c payload/source/rf_api.c payload/source/rf_main.c payload/source/remote_stock_icons.c payload/source/custom_icon_registry.c payload/source/icon_pack.c payload/source/timezone_db.c payload/source/sha256.c payload/source/ota_verify.c payload/source/tweetnacl.c"
    "$CC" $CFLAGS $OTA_KEY_CFLAGS -Ipayload/source -o "$BIN/hub_rf" $RF_SRCS payload/source/cJSON.c payload/source/uclibc_static_locks.c $COMPAT_SRC -lpthread -lm
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
    # The scripts are part of the package: a missing one stops the build (the remote's installer
    # would otherwise keep its old copy next to the new remote_ui, with no error anywhere)
    cp -f "$REPO_ROOT/remote/rcS.local" "$REPO_ROOT/remote/init.sh" "$REPO_ROOT/remote/remote_watchdog.sh" "$REPO_ROOT/remote/factory_reset.sh" "$REPO_ROOT/remote/safe_reboot.sh" "$REPO_ROOT/build/tmp/remote_ota_pkg/"
    sed -i 's/\r$//' "$REPO_ROOT/build/tmp/remote_ota_pkg"/*.sh "$REPO_ROOT/build/tmp/remote_ota_pkg"/rcS.local
    # Theme packages. Building them needs a host C compiler and the pinned downloads; without
    # them, themes built earlier into build/tmp/remote-themes are used on a developer machine.
    # In CI (CI=true) or with REQUIRE_THEMES=1 a failed theme build stops the build.
    echo "[-] Building remote themes..."
    if ! python3 "$REPO_ROOT/tools/remote_render/build_theme.py" >/dev/null; then
        if [ "${CI:-}" = "true" ] || [ "${REQUIRE_THEMES:-0}" = "1" ]; then
            echo "[!] Theme build failed." >&2
            exit 1
        fi
        echo "[!] Theme build failed; packaging themes from an earlier build, if any (they may not match this build)." >&2
    fi
    rm -rf "$REPO_ROOT/remote/bin/themes"
    if [ -d "$REPO_ROOT/build/tmp/remote-themes" ]; then
        cp -R "$REPO_ROOT/build/tmp/remote-themes" "$REPO_ROOT/remote/bin/themes"
        cp -R "$REPO_ROOT/build/tmp/remote-themes" "$REPO_ROOT/build/tmp/remote_ota_pkg/themes"
    fi
    tar -czf "$REPO_ROOT/remote/bin/remote_ui.ota.gz" -C "$REPO_ROOT/build/tmp/remote_ota_pkg" .
    cp -f "$REPO_ROOT/remote/bin/remote_ui.ota.gz" "$REPO_ROOT/remote/bin/remote_ui" "$REPO_ROOT/remote/bin/remote_sync" "$REPO_ROOT/build/output/remote/"
    rm -rf "$REPO_ROOT/build/output/remote/themes"
    [ -d "$REPO_ROOT/remote/bin/themes" ] && cp -R "$REPO_ROOT/remote/bin/themes" "$REPO_ROOT/build/output/remote/themes"
    rm -rf "$REPO_ROOT/build/tmp/remote_ota_pkg"
}


case "$TARGET" in
    remote|elite)
        build_remote
        rm -f "$BUILD_STAMP"
        exit 0
        ;;
    daemon)
        build_daemon
        ;;
    bearssl)
        build_bearssl
        ;;
    fetch|hub_fetch)
        build_fetch
        ;;
    util|hub_util)
        build_util
        ;;
    sntp)
        build_sntp
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
        rm -f "$BUILD_STAMP"
        exit 0
        ;;
    eth_fix)
        build_eth_fix
        ;;
    hub)
        build_util
        build_daemon
        build_fetch
        build_rf
        ;;
    all)
        build_util
        build_daemon
        build_fetch
        build_rf
        build_btstack
        ;;
    *)
        echo "Unknown target: $TARGET"
        echo "Usage: $0 [all|hub|daemon|util|fetch|bearssl|sntp|btstack|register_ehci|eth_fix|mknod|rf|remote|apk] (elite = remote)"
        exit 1
        ;;
esac

echo "Copying binaries to build/output..."
# copied when it differs: a tree built before kept shipping its older server
if [ -f "$REPO_ROOT/tools/bin/dropbearmulti_mips" ] && ! cmp -s "$REPO_ROOT/tools/bin/dropbearmulti_mips" "$BIN/dropbearmulti"; then
    cp -f "$REPO_ROOT/tools/bin/dropbearmulti_mips" "$BIN/dropbearmulti"
fi
# Only what this run built (plus the dropbear blob, which is never built here): a binary that
# an earlier build left in payload/bin is not copied, and its old copy in build/output goes,
# so build/output and MANIFEST.txt describe one build
BUILT=""
for f in hub_util hub_fetch hub_btstack hub_daemon hub_sntp hub_rf hub_sync sync register_ehci eth_fix mknod hub_usbwifi dropbearmulti; do
    if [ -e "$BIN/$f" ] && { [ "$f" = dropbearmulti ] || [ "$BIN/$f" -nt "$BUILD_STAMP" ] || \
            { [ -L "$BIN/$f" ] && [ "$BIN/$(readlink "$BIN/$f")" -nt "$BUILD_STAMP" ]; }; }; then
        cp -a "$BIN/$f" "$OUT/" 2>/dev/null || cp -f "$BIN/$f" "$OUT/"
        BUILT="$BUILT $f"
    else
        rm -f "$OUT/$f"
    fi
done
rm -f "$BUILD_STAMP"

echo "Updating MANIFEST.txt and FILES..."
cd "$BIN"
if [ -n "$BUILT" ]; then
    if command -v file >/dev/null 2>&1; then
        file $BUILT > FILES
    fi
    ls -l $BUILT | sed -E 's/ [a-zA-Z0-9_-]+ [a-zA-Z0-9_-]+ / root root /' > MANIFEST.txt
    md5sum $BUILT >> MANIFEST.txt
fi

echo "=== Build finished successfully! ==="
ls -lh "$BIN"/hub_* "$BIN"/register_ehci "$BIN"/mknod 2>/dev/null || true
