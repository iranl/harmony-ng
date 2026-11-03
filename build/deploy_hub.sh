#!/bin/bash
set -euo pipefail

HUB_HOST=""
HUB_USER="root"
DEBUG_BUSYBOX="${DEBUG_BUSYBOX:-0}"
SSH_KEY="${SSH_KEY:-}"

REBOOT_HUB=0
UI_CHOICE="${UI_CHOICE:-legacy}"
WEBUI_ONLY=0

# Parse optional flags
for arg in "$@"; do
    case "$arg" in
        --debug|--debug-busybox|--dev)
            DEBUG_BUSYBOX=1
            ;;
        --reboot)
            REBOOT_HUB=1
            ;;
        --ui=*)
            UI_CHOICE="${arg#*=}"
            ;;
        --ui-legacy|--legacy-ui|--old-ui)
            UI_CHOICE="legacy"
            ;;
        --ui-new|--new-ui)
            UI_CHOICE="new"
            ;;
        --webui-only|--ui-only)
            WEBUI_ONLY=1
            ;;
        --host=*|-h=*)
            HUB_HOST="${arg#*=}"
            ;;
        --key=*|-k=*)
            SSH_KEY="${arg#*=}"
            ;;
        --help|-h)
            echo "usage: $0 HUB_ADDRESS [--webui-only] [--ui=new|legacy] [--reboot] [--debug] [--key=PATH]"
            exit 0
            ;;
        -*)
            echo "[-] Unknown option: $arg" >&2
            exit 1
            ;;
        *)
            # The Hub's address: the first argument that isn't a flag
            [ -z "$HUB_HOST" ] && HUB_HOST="$arg"
            ;;
    esac
done
if [ -z "$HUB_HOST" ]; then
    echo "usage: $0 HUB_ADDRESS [--webui-only] [--ui=new|legacy] [--reboot] [--debug] [--key=PATH]" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PAYLOAD_DIR="$REPO_ROOT/payload"
if [ -d "$SCRIPT_DIR/bin" ]; then
    BIN_DIR="$SCRIPT_DIR/bin"
    WWW_DIR="$SCRIPT_DIR/www"
    SCRIPTS_DIR="$SCRIPT_DIR/scripts"
    MODULES_DIR="$SCRIPT_DIR/modules"
    CONFIG_DIR="$SCRIPT_DIR/config"
    FW_TOOL="$SCRIPT_DIR/tools/harmony_firmware.py"
    ICONS_TAR="$SCRIPT_DIR/icons.tar.gz"
else
    BIN_DIR="$REPO_ROOT/payload/bin"
    WWW_DIR="$REPO_ROOT/payload/www"
    SCRIPTS_DIR="$REPO_ROOT/payload/scripts"
    MODULES_DIR="$REPO_ROOT/payload/modules"
    CONFIG_DIR="$REPO_ROOT/payload/config"
    FW_TOOL="$REPO_ROOT/tools/harmony_firmware.py"
    ICONS_TAR="$REPO_ROOT/build/icons.tar.gz"
fi
if [ -f "$REPO_ROOT/build/icons.tar.gz" ]; then
    ICONS_TAR="$REPO_ROOT/build/icons.tar.gz"
elif [ -f "$SCRIPT_DIR/icons.tar.gz" ]; then
    ICONS_TAR="$SCRIPT_DIR/icons.tar.gz"
fi

# SSH key: --key=PATH, $SSH_KEY, else the newest ~/.ssh/harmony_owner_* (authorized by root_hub_usb.py), id_rsa, id_ed25519
if [ -z "$SSH_KEY" ]; then
    SSH_KEY="$(ls -t "$HOME"/.ssh/harmony_owner_* 2>/dev/null | grep -v '\.pub$' | head -n 1 || true)"
fi
for cand in "$HOME/.ssh/id_rsa" "$HOME/.ssh/id_ed25519"; do
    if [ -z "$SSH_KEY" ] && [ -f "$cand" ]; then
        SSH_KEY="$cand"
    fi
done
SSH_OPTS=(-q -o BatchMode=yes -o StrictHostKeyChecking=no)
if [ -n "$SSH_KEY" ]; then
    SSH_OPTS+=(-i "$SSH_KEY")
fi

echo "=== Harmony Hub Firmware Deployment ==="
echo "Target Hub: $HUB_HOST"
echo "SSH key: ${SSH_KEY:-(ssh defaults)}"
if [ "$DEBUG_BUSYBOX" = "1" ]; then
    echo "Mode: Developer / Debug (Extended BusyBox enabled)"
else
    echo "Mode: Standard User (Stock BusyBox; existing installation left untouched)"
fi

# Uploads a file when its MD5 differs from the Hub's copy in $REMOTE_MD5_LIST, which each
# deployment mode reads from the Hub before its first upload
if command -v md5sum >/dev/null 2>&1; then
    compute_md5() { md5sum "$1" | awk '{print $1}'; }
else
    compute_md5() { md5 -q "$1"; }  # macOS before 15 ships md5 but not md5sum
fi

deploy_file_if_changed() {
    local src="$1"
    local dst="$2"
    local perm="${3:-755}"
    [ ! -f "$src" ] && return 0
    local local_md5
    local_md5=$(compute_md5 "$src")
    local remote_md5
    remote_md5=$(echo "$REMOTE_MD5_LIST" | awk -v p="$dst" '$2 == p {print $1; exit}')
    local fname
    fname=$(basename "$src")
    if [ -n "$remote_md5" ] && [ "$remote_md5" = "$local_md5" ]; then
        echo "    -> $fname (MD5 match: $local_md5 - skipped)"
        return 0
    fi
    echo "    -> Uploading $fname (local: $local_md5 != remote: ${remote_md5:-missing})..."
    local ddir
    ddir=$(dirname "$dst")

    # Failures are collected in DEPLOY_FAILURES and reported after the services restart, rather
    # than stopping the deployment while they are stopped.
    # The Hub's flash filesystems are small (/data is 5 MB). The new copy goes next to the old one
    # as $dst.new when both fit; otherwise it is staged in RAM (/tmp) and replaces the old copy,
    # which frees its space because the services using it are stopped. Either way the copy is
    # checked against the local MD5 before it replaces anything. Stock BusyBox 1.13.4 lacks df, so
    # without the extended BusyBox the free space is unknown and the side-by-side copy is tried.
    local size_kb avail_kb old_kb
    size_kb=$(( ($(wc -c < "$src") + 1023) / 1024 ))
    read -r avail_kb old_kb < <(ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
        "mkdir -p '$ddir' 2>/dev/null; rm -f '${dst}.new' 2>/dev/null; a=\$(df -k '$ddir' 2>/dev/null | awk 'END {print \$(NF-2)}'); o=0; [ -f '$dst' ] && o=\$(ls -l '$dst' 2>/dev/null | awk '{print int((\$5+1023)/1024)}'); echo \"\${a:-unknown} \${o:-0}\"") || true
    case "${avail_kb:-}" in ''|*[!0-9]*) avail_kb="" ;; esac  # unknown: try the side-by-side copy
    case "${old_kb:-}" in ''|*[!0-9]*) old_kb=0 ;; esac
    local need_kb=$((size_kb + 32))  # flash filesystems need room beyond the file's own size
    local stage
    if [ -z "${avail_kb:-}" ] || [ "$avail_kb" -ge "$need_kb" ]; then
        stage="${dst}.new"
    elif [ $((avail_kb + old_kb)) -ge "$need_kb" ]; then
        stage="/tmp/deploy.$fname"
        echo "       Not enough space for both copies (${avail_kb} KB free, ${size_kb} KB needed); replacing in place."
    else
        echo "    [-] $fname: $ddir has ${avail_kb} KB free and the old copy frees ${old_kb} KB, but ${need_kb} KB is needed. Free space on the Hub and deploy again." >&2
        DEPLOY_FAILURES+=("$dst (no space)")
        return 0
    fi

    if ! scp -O "${SSH_OPTS[@]}" "$src" "${HUB_USER}@${HUB_HOST}:${stage}"; then
        ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "rm -f '$stage' 2>/dev/null"
        echo "    [-] $fname: upload failed; the Hub keeps its old copy." >&2
        DEPLOY_FAILURES+=("$dst (upload failed)")
        return 0
    fi
    local result
    result=$(ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
        "s='$stage'; d='$dst'; m=\$(md5sum \"\$s\" 2>/dev/null | cut -d' ' -f1); \
         if [ \"\$m\" != '$local_md5' ]; then rm -f \"\$s\"; echo staged-mismatch; exit; fi; \
         chmod $perm \"\$s\" 2>/dev/null; \
         if [ \"\$s\" = \"\$d.new\" ]; then mv -f \"\$s\" \"\$d\" && echo ok || echo move-failed; exit; fi; \
         rm -f \"\$d\"; cp \"\$s\" \"\$d\" 2>/dev/null; m=\$(md5sum \"\$d\" 2>/dev/null | cut -d' ' -f1); \
         if [ \"\$m\" = '$local_md5' ]; then chmod $perm \"\$d\" 2>/dev/null; rm -f \"\$s\"; echo ok; else rm -f \"\$d\"; echo copy-failed; fi")
    case "$result" in
        ok) return 0 ;;
        staged-mismatch)
            echo "    [-] $fname: the uploaded copy doesn't match the local file; the Hub keeps its old copy." >&2 ;;
        copy-failed)
            echo "    [-] $fname: writing $dst failed and the old copy is gone. The new copy is in $stage on the Hub until it reboots; free space and deploy again." >&2 ;;
        *)
            echo "    [-] $fname: replacing $dst failed (${result:-no reply})." >&2 ;;
    esac
    DEPLOY_FAILURES+=("$dst (${result:-no reply})")
    return 0
}
DEPLOY_FAILURES=()

if [ "$WEBUI_ONLY" = "0" ]; then
# 1. Run automated backup first
echo "[1/5] Running pre-deployment backup..."
SSH_KEY="$SSH_KEY" bash "$SCRIPT_DIR/backup_hub.sh" "$HUB_HOST"

# Keep the stock startup script so a factory reset can restore it (same as deploy_hub.ps1)
ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'elite' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'harmony' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; echo '[+] Saved /etc/init.d/rcS.local to /data/stock_backup/rcS.local.orig'; fi; fi"

# 2. Stop existing processes cleanly (quiescing RF & SPI flash). v1.x runs codex_* binaries and
#    scripts from /data/codex, which would keep port 8089 and /dev/rfspi from the new services.
echo "[2/5] Stopping old hub processes cleanly..."
ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "touch /tmp/harmony_stop 2>/dev/null || true; if [ -x /data/harmony/init.sh ]; then /data/harmony/init.sh stop; elif [ -x /data/harmony/scripts/init.sh ]; then /data/harmony/scripts/init.sh stop; else killall -15 hub_rf 2>/dev/null || true; sleep 1; killall -9 hub_rf hub_daemon hub_btstack hub_dhcpd hub_portal hal luaworks luadraws lua 2>/dev/null || true; fi; for _p in \$(ps | grep '/data/codex/' | sed 's/^[ ]*//' | cut -d' ' -f1 2>/dev/null); do [ -n \"\$_p\" ] && [ \"\$_p\" != \"\$\$\" ] && kill -9 \"\$_p\" 2>/dev/null || true; done; for _p in \$(ps | grep 'init\.sh' | sed 's/^[ ]*//' | cut -d' ' -f1 2>/dev/null); do [ -n \"\$_p\" ] && [ \"\$_p\" != \"\$\$\" ] && kill -9 \"\$_p\" 2>/dev/null || true; done; /data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true"

# 3. One-way migration from /data/codex to /data/harmony and cleanup of old binaries
echo "[3/5] Migrating legacy codex paths and querying remote checksums..."
ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "if [ -d /data/codex ] && [ ! -L /data/codex ]; then if [ ! -d /data/harmony ]; then mv /data/codex /data/harmony 2>/dev/null || true; else cp -rn /data/codex/* /data/harmony/ 2>/dev/null || true; rm -rf /data/codex 2>/dev/null || true; fi; fi; rm -f /data/codex /data/codex_p 2>/dev/null || true; [ -f /data/harmony/mqtt_config.json ] && [ ! -f /data/harmony/mqtt.json ] && mv -f /data/harmony/mqtt_config.json /data/harmony/mqtt.json 2>/dev/null || true; [ -f /data/harmony/elite_remote_map.json ] && [ ! -f /data/harmony/remote_slot1_map.json ] && mv -f /data/harmony/elite_remote_map.json /data/harmony/remote_slot1_map.json 2>/dev/null || true; [ -f /data/harmony/bt_devices.json ] && [ ! -f /data/harmony/bt-devices.json ] && mv -f /data/harmony/bt_devices.json /data/harmony/bt-devices.json 2>/dev/null || true; rm -rf /data/codex-backups /data/codexmqtt /data/harmony-backups /cache/crashlog-*.json 2>/dev/null; rm -f /data/harmony/mqtt_config.json /data/harmony/elite_remote_map.json /data/harmony/bt_devices.json /data/devices.json /data/activities.json /data/harmony/devices.json /data/harmony/config/.imported /data/elite_ui.ota.gz /data/elite_ui.version 2>/dev/null || true; rm -f /cache/codex_*.ota.gz /cache/codex_*.ota /cache/*.new /cache/*/*.new /data/harmony/*.new /data/harmony/*/*.new /data/rf_pair_test /data/test_hub_tx /data/codex_rf /data/hub_rf /data/harmony/bin/codex_* /cache/bin/codex_* /var/run/codex_* 2>/dev/null || true; mkdir -p /data/harmony/bin /data/harmony/www/assets /data/harmony/config /data/harmony/custom_icons"

# If no devices.json exists yet, check if stock Logitech resources are present and migrate them
HAS_DEV=$(ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "[ -f /data/harmony/config/devices.json ] && echo 1 || echo 0" 2>/dev/null || echo 0)
if [ "$HAS_DEV" = "0" ]; then
    HAS_STOCK=$(ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "[ -f /data/resources/DeviceList.json ] && echo 1 || echo 0" 2>/dev/null || echo 0)
    if [ "$HAS_STOCK" = "1" ] && [ -f "$REPO_ROOT/tools/migrate_hub_configs.py" ]; then
        echo "[*] Migrating stock Logitech configurations to canonical v2.0 format..."
        TMP_MIG=$(mktemp -d 2>/dev/null || mktemp -d -t 'harmig')
        scp -O -r "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}:/data/resources" "$TMP_MIG/" 2>/dev/null || true
        PY_BIN="python3"
        command -v python3 >/dev/null 2>&1 || PY_BIN="python"
        $PY_BIN "$REPO_ROOT/tools/migrate_hub_configs.py" --src "$TMP_MIG/resources" --dest "$TMP_MIG/out" 2>/dev/null || true
        if [ -f "$TMP_MIG/out/config/devices.json" ]; then
            scp -O "${SSH_OPTS[@]}" "$TMP_MIG/out/config/"* "${HUB_USER}@${HUB_HOST}:/data/harmony/config/" 2>/dev/null || true
            for _mf in "$TMP_MIG/out/"*.json; do
                [ -f "$_mf" ] && scp -O "${SSH_OPTS[@]}" "$_mf" "${HUB_USER}@${HUB_HOST}:/data/harmony/" 2>/dev/null || true
            done
            echo "[+] Stock configurations converted and uploaded successfully."
        fi
        rm -rf "$TMP_MIG" 2>/dev/null || true
    fi
fi

REMOTE_MD5_LIST=$(ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "md5sum /data/harmony/bin/* /data/harmony/* /cache/www/* /cache/www/assets/* /cache/www/assets/themes/* /cache/modules/* /cache/bin/busybox /data/harmony/elite_ui.ota.gz /cache/icons.tar.gz /etc/init.d/rcS.local 2>/dev/null || true")

for bin in hub_daemon hub_rf hub_sntp hub_btstack hub_sync hub_dhcpd hub_portal mknod register_ehci eth_fix; do
    if [ -f "$BIN_DIR/$bin" ]; then
        deploy_file_if_changed "$BIN_DIR/$bin" "/data/harmony/bin/$bin" 755
    fi
done

# Extended BusyBox (default deployment)
if [ -f "$BIN_DIR/busybox" ]; then
    echo "[*] Syncing extended BusyBox to /cache/bin/..."
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "mkdir -p /cache/bin 2>/dev/null"
    deploy_file_if_changed "$BIN_DIR/busybox" "/cache/bin/busybox" 755
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "ln -sf /cache/bin/busybox /data/harmony/bin/busybox 2>/dev/null; /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true; for _app in \$(/cache/bin/busybox --list 2>/dev/null); do [ ! -e \"/bin/\$_app\" ] && [ ! -e \"/usr/bin/\$_app\" ] && ln -sf /cache/bin/busybox \"/usr/bin/\$_app\" 2>/dev/null || true; done; mkdir -p /etc/profile.d 2>/dev/null; echo 'export PATH=/cache/bin:/data/harmony/bin:\$PATH' > /etc/profile.d/00-harmony.sh 2>/dev/null; chmod 644 /etc/profile.d/00-harmony.sh 2>/dev/null || true"
fi
# Ensure root profile uses extended BusyBox in cache if present
ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "for prof in /root/.profile /home/root/.profile; do mkdir -p \"\$(dirname \"\$prof\")\" 2>/dev/null; printf \"BB_PATH=\\\"\\\"\n[ -x /cache/bin/busybox ] && BB_PATH=/cache/bin/busybox\n[ -x /cache/busybox ] && BB_PATH=/cache/busybox\nif [ -t 0 ] && [ -n \\\"\\\$BB_PATH\\\" ] && [ -z \\\"\\\$BB_EXTENDED\\\" ]; then\n  export BB_EXTENDED=1\n  export PATH=/cache/bin:/data/harmony/bin:\\\$PATH\n  exec \\\"\\\$BB_PATH\\\" sh -l\nfi\nexport PATH=/cache/bin:/data/harmony/bin:\\\$PATH\n\" > \"\$prof\"; chmod 644 \"\$prof\" 2>/dev/null || true; done"

# Deploy payload scripts
if [ -d "$SCRIPTS_DIR" ]; then
    echo "[*] Syncing startup & network scripts to /data/harmony/..."
    for sc in "$SCRIPTS_DIR"/*; do
        [ -f "$sc" ] || continue
        deploy_file_if_changed "$sc" "/data/harmony/$(basename "$sc")" 755
    done
fi

if [ -f "$REPO_ROOT/VERSION" ]; then
    echo "[*] Syncing VERSION file to /data/harmony/VERSION..."
    deploy_file_if_changed "$REPO_ROOT/VERSION" "/data/harmony/VERSION" 644
fi

# Deploy Elite RF OTA image if available
ELITE_OTA_SRC="$REPO_ROOT/elite/bin/elite_ui.ota.gz"
[ ! -f "$ELITE_OTA_SRC" ] && ELITE_OTA_SRC="$REPO_ROOT/build/output/elite/elite_ui.ota.gz"
if [ -f "$ELITE_OTA_SRC" ]; then
    echo "[*] Syncing Elite RF OTA image (/data/harmony/elite_ui.ota.gz)..."
    deploy_file_if_changed "$ELITE_OTA_SRC" "/data/harmony/elite_ui.ota.gz" 644
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "rm -f /cache/elite_ui.ota.gz 2>/dev/null || true; cp -f /data/harmony/VERSION /data/harmony/elite_ui.version 2>/dev/null || true"
fi

# Deploy config files (e.g. curated CA bundle)
if [ -d "$CONFIG_DIR" ]; then
    echo "[*] Syncing config files to /data/harmony/config/..."
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "mkdir -p /data/harmony/config 2>/dev/null || true"
    for cfg in "$CONFIG_DIR"/*; do
        [ -f "$cfg" ] || continue
        cname=$(basename "$cfg")
        deploy_file_if_changed "$cfg" "/data/harmony/config/$cname" 644
        if [ "$cname" = "cacert.pem" ]; then
            ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "ln -sf /data/harmony/config/cacert.pem /data/harmony/cacert.pem 2>/dev/null || true"
        fi
    done
fi

# Deploy USB Ethernet kernel modules to /cache/modules (symlinked to /data/harmony/modules)
if [ -d "$MODULES_DIR" ]; then
    echo "[*] Syncing USB Ethernet kernel modules to /cache/modules/..."
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "mkdir -p /cache/modules 2>/dev/null; [ -L /data/harmony/modules ] || ln -sf /cache/modules /data/harmony/modules"
    for mod in "$MODULES_DIR"/*.ko; do
        [ -f "$mod" ] || continue
        deploy_file_if_changed "$mod" "/cache/modules/$(basename "$mod")" 644
    done
fi

    # Install the HarmonyNG startup script; without it the Hub boots the stock stack on next reboot
    if [ -f "$SCRIPTS_DIR/rcS.local" ]; then
        echo "[*] Installing startup script /etc/init.d/rcS.local..."
        deploy_file_if_changed "$SCRIPTS_DIR/rcS.local" "/etc/init.d/rcS.local" 755
    fi
else
    echo "[*] WebUI-only deployment mode active (--webui-only)."
    REMOTE_MD5_LIST=$(ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "md5sum /cache/www/* /cache/www/assets/* /cache/www/assets/themes/* /cache/icons.tar.gz 2>/dev/null || true")
fi

# 4. Deploy WebUI to /cache/www/ (preserves critical /data flash)
echo "[4/5] Building & syncing WebUI ($UI_CHOICE) to /cache/www/ (MD5 check)..."
if [ "$WWW_DIR" = "$REPO_ROOT/payload/www" ] && [ -d "$REPO_ROOT/frontend" ]; then
    UI_SRC_DIR="$REPO_ROOT/frontend"
    if [ "$UI_CHOICE" = "legacy" ] || [ "$UI_CHOICE" = "old" ] || [ "$UI_CHOICE" = "android-app" ]; then
        UI_SRC_DIR="$REPO_ROOT/android-app"
    fi
    # The Elite page's screen preview is the remote's renderer built to WebAssembly; without
    # Emscripten the page leaves the preview out
    if command -v emcc >/dev/null 2>&1; then
        "$REPO_ROOT/tools/elite_render/build.sh" --wasm "$UI_SRC_DIR/public/assets" >/dev/null \
            || echo "[!] Elite screen renderer build failed; the Elite page will have no screen preview."
        python3 "$REPO_ROOT/tools/elite_render/build_theme.py" --tar "$UI_SRC_DIR/public/assets/themes" >/dev/null \
            || echo "[!] Elite theme build failed; the screen preview will show Classic only."
    fi
    echo "[*] Compiling frontend bundle with Vite ($UI_SRC_DIR)..."
    # A failed build stops here, before the last good copy in $WWW_DIR is emptied
    if ! (cd "$UI_SRC_DIR" && npm run build) || [ ! -f "$UI_SRC_DIR/dist/index.html" ]; then
        echo "[-] WebUI build failed in $UI_SRC_DIR (run 'npm install' there?); nothing was deployed from this step." >&2
        exit 1
    fi
    mkdir -p "$WWW_DIR"
    find "$WWW_DIR" -maxdepth 1 -type f -delete 2>/dev/null || true
    rm -rf "$WWW_DIR/assets" 2>/dev/null || true
    cp -rf "$UI_SRC_DIR/dist"/* "$WWW_DIR/"
    rm -rf "$WWW_DIR/assets/icons" 2>/dev/null || true
fi

if [ -f "$REPO_ROOT/build/gzip_assets.py" ]; then
    python3 "$REPO_ROOT/build/gzip_assets.py" "$WWW_DIR" 2>/dev/null || true
fi

# Clean up physical directory on /data if present, symlink to /cache/www, prune flash icons
ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "mkdir -p /cache/www/assets 2>/dev/null; rm -rf /cache/www/assets/icons 2>/dev/null; if [ -d /data/harmony/www ] && [ ! -L /data/harmony/www ]; then rm -rf /data/harmony/www 2>/dev/null; fi; mkdir -p /data/harmony 2>/dev/null; [ -L /data/harmony/www ] || ln -sfn /cache/www /data/harmony/www 2>/dev/null"

# Clean up stale assets on Hub (/cache/www/assets)
for remote_file in $(ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "ls -1 /cache/www/assets/ 2>/dev/null || true"); do
    if [ ! -f "$WWW_DIR/assets/$remote_file" ]; then
        echo "    [-] Purging obsolete asset: $remote_file"
        ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "rm -f /cache/www/assets/$remote_file 2>/dev/null || true"
    fi
done

while IFS= read -r -d '' wfile; do
    rel="${wfile#$WWW_DIR/}"
    deploy_file_if_changed "$wfile" "/cache/www/$rel" 644
done < <(find "$WWW_DIR" -type f -print0)

if [ ! -f "$ICONS_TAR" ] && [ -f "$FW_TOOL" ]; then
    echo ""
    echo "[?] $ICONS_TAR not found on host."
    read -p "Download official Elite firmware, extract 1,350 icons, and build $ICONS_TAR now? [Y/n] " -r REPLY || true
    if [ -z "$REPLY" ] || [[ "$REPLY" =~ ^[Yy]$ ]]; then
        echo "[*] Extracting remote UI icons and packaging $ICONS_TAR..."
        python3 "$FW_TOOL" extract-icons --archive "$ICONS_TAR" || true
    fi
    if [ ! -f "$ICONS_TAR" ]; then
        echo "[!] No icon archive: the Hub's icons are left as they are. Copy an icons.tar.gz from a"
        echo "    release package to $ICONS_TAR, or build it from a local bundle with:"
        echo "    python3 $FW_TOOL extract-icons --firmware PATH.hfw2 --archive $ICONS_TAR"
    fi
fi

if [ -f "$ICONS_TAR" ]; then
    deploy_file_if_changed "$ICONS_TAR" "/cache/icons.tar.gz" 644
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
        "if [ -f /data/harmony/icons.tar.gz ] && [ ! -L /data/harmony/icons.tar.gz ]; then rm -f /data/harmony/icons.tar.gz 2>/dev/null; fi; [ -L /data/harmony/icons.tar.gz ] || ln -sf /cache/icons.tar.gz /data/harmony/icons.tar.gz 2>/dev/null"
fi

ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "chmod 755 /cache/www /cache/www/assets 2>/dev/null || true"

# 5. Post-deployment: safe reboot OR hot service restart (default)
if [ "$REBOOT_HUB" = "1" ]; then
    echo "[5/5] Verifying system shell and flushing NOR flash buffers (rebooting)..."
    ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
        'if ! /bin/sh -c "true" 2>/dev/null; then echo "[-] CRITICAL: /bin/sh verification failed. Aborting reboot!"; exit 1; fi; chmod +x /data/harmony/safe_reboot.sh 2>/dev/null || true; ln -sf /data/harmony/safe_reboot.sh /data/harmony/bin/safe_reboot.sh 2>/dev/null || true; ln -sf /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true; (sleep 2 && /data/harmony/safe_reboot.sh) >/dev/null 2>&1 &'
    echo "=== Deployment to $HUB_HOST Complete! Hub is rebooting safely. ==="
else
    echo "[5/5] Flushing NOR flash buffers and hot-restarting services (no reboot)..."
    ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
        'chmod +x /data/harmony/bin/* /data/harmony/*.sh /data/harmony/init.sh 2>/dev/null || true; mkdir -p /data/harmony/scripts 2>/dev/null || true; ln -sf /data/harmony/init.sh /data/harmony/scripts/init.sh 2>/dev/null || true; ln -sf /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true; ln -sf /data/harmony/bin/hub_sync /cache/bin/sync 2>/dev/null || true; /data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true; rm -f /tmp/harmony_stop 2>/dev/null || true; if [ -x /data/harmony/init.sh ]; then /data/harmony/init.sh start >/dev/null 2>&1 & fi'
    sleep 3
    PROCS=$(ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "pidof hub_daemon hub_rf hub_btstack 2>/dev/null || true")
    if [ -n "$PROCS" ]; then
        echo "=== Deployment to $HUB_HOST Complete! Hub services running (PIDs: $PROCS). ==="
    else
        echo "=== Deployment to $HUB_HOST Complete! Services started. ==="
    fi
fi

if [ "${#DEPLOY_FAILURES[@]}" -gt 0 ]; then
    echo "[-] These files were not deployed; the Hub runs with its old copies:" >&2
    for f in "${DEPLOY_FAILURES[@]}"; do echo "    $f" >&2; done
    exit 1
fi
