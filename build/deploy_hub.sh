#!/bin/bash
set -euo pipefail

HUB_HOST="${1:-192.168.1.1}"
HUB_USER="root"
DEBUG_BUSYBOX="${DEBUG_BUSYBOX:-0}"
SSH_KEY="${SSH_KEY:-}"

# Parse optional flags
for arg in "$@"; do
    case "$arg" in
        --debug|--debug-busybox|--dev)
            DEBUG_BUSYBOX=1
            ;;
        --host=*|-h=*)
            HUB_HOST="${arg#*=}"
            ;;
        --key=*|-k=*)
            SSH_KEY="${arg#*=}"
            ;;
        192.*|10.*|172.*|*.local)
            HUB_HOST="$arg"
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
if [ -d "$SCRIPT_DIR/bin" ]; then
    BIN_DIR="$SCRIPT_DIR/bin"
    WWW_DIR="$SCRIPT_DIR/www"
    SCRIPTS_DIR="$SCRIPT_DIR/scripts"
    MODULES_DIR="$SCRIPT_DIR/modules"
    FW_TOOL="$SCRIPT_DIR/tools/harmony_firmware.py"
    ICONS_TAR="$SCRIPT_DIR/icons.tar.gz"
else
    BIN_DIR="$REPO_ROOT/payload/bin"
    WWW_DIR="$REPO_ROOT/payload/www"
    SCRIPTS_DIR="$REPO_ROOT/payload/scripts"
    MODULES_DIR="$REPO_ROOT/payload/modules"
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

# 1. Run automated backup first
echo "[1/5] Running pre-deployment backup..."
SSH_KEY="$SSH_KEY" bash "$SCRIPT_DIR/backup_hub.sh" "$HUB_HOST"

# Keep the stock startup script so a factory reset can restore it (same as deploy_hub.ps1)
ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'elite' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'harmony' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; echo '[+] Saved /etc/init.d/rcS.local to /data/stock_backup/rcS.local.orig'; fi; fi"

# 2. Stop existing processes with kill -9
echo "[2/5] Stopping old hub processes..."
ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "killall -9 hub_daemon codex_daemon hub_rf codex_rf hub_btstack codex_btstack hub_dhcpd codex_dhcpd hub_portal codex_portal hal luaworks luadraws lua 2>/dev/null || true; sleep 1"

# 3. One-way migration from /data/codex to /data/harmony and cleanup of old binaries
echo "[3/5] Migrating legacy codex paths and querying remote checksums..."
ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "if [ -d /data/codex ] && [ ! -L /data/codex ]; then if [ ! -d /data/harmony ]; then mv /data/codex /data/harmony 2>/dev/null || true; else cp -rn /data/codex/* /data/harmony/ 2>/dev/null || true; rm -rf /data/codex 2>/dev/null || true; fi; fi; rm -f /data/codex /data/codex_p 2>/dev/null || true; rm -rf /data/codex-backups /data/codexmqtt /data/harmony-backups /cache/crashlog-*.json 2>/dev/null; rm -f /cache/codex_*.ota.gz /cache/codex_*.ota /cache/*.new /cache/*/*.new /data/harmony/*.new /data/harmony/*/*.new /data/rf_pair_test /data/test_hub_tx /data/codex_rf /data/hub_rf /data/harmony/bin/codex_* /cache/bin/codex_* /var/run/codex_* 2>/dev/null || true; mkdir -p /data/harmony/bin /data/harmony/www/assets /data/harmony/config /data/harmony/custom_icons"

REMOTE_MD5_LIST=$(ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "md5sum /data/harmony/bin/* /data/harmony/* /cache/www/* /cache/www/assets/* /cache/modules/* /cache/bin/busybox /cache/elite_ui.ota.gz /cache/icons.tar.gz /etc/init.d/rcS.local 2>/dev/null || true")

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
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "mkdir -p '$ddir' 2>/dev/null || true"
    scp -O "${SSH_OPTS[@]}" "$src" "${HUB_USER}@${HUB_HOST}:${dst}.new"
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "chmod $perm ${dst}.new 2>/dev/null && mv -f ${dst}.new $dst 2>/dev/null || true"
}

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

# 4. Deploy WebUI to /cache/www/ (preserves critical /data flash)
echo "[4/5] Syncing WebUI to /cache/www/ (MD5 check)..."
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
fi

if [ -f "$ICONS_TAR" ]; then
    deploy_file_if_changed "$ICONS_TAR" "/cache/icons.tar.gz" 644
    ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
        "if [ -f /data/harmony/icons.tar.gz ] && [ ! -L /data/harmony/icons.tar.gz ]; then rm -f /data/harmony/icons.tar.gz 2>/dev/null; fi; [ -L /data/harmony/icons.tar.gz ] || ln -sf /cache/icons.tar.gz /data/harmony/icons.tar.gz 2>/dev/null"
fi

ssh -n "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "chmod 755 /cache/www /cache/www/assets 2>/dev/null || true"

# 5. Pre-reboot integrity check & safe reboot (flush SPI NOR flash)
echo "[5/5] Verifying system shell and flushing NOR flash buffers..."
ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    'if ! /bin/sh -c "true" 2>/dev/null; then echo "[-] CRITICAL: /bin/sh verification failed. Aborting reboot!"; exit 1; fi; chmod +x /data/harmony/safe_reboot.sh 2>/dev/null || true; ln -sf /data/harmony/safe_reboot.sh /data/harmony/bin/safe_reboot.sh 2>/dev/null || true; ln -sf /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true; ln -sf /data/harmony/bin/hub_sync /cache/bin/sync 2>/dev/null || true; killall -9 init.sh hub_daemon codex_daemon hub_btstack codex_btstack hub_rf codex_rf hub_dhcpd codex_dhcpd hub_portal codex_portal hal luaworks luadraws lua 2>/dev/null || true; sleep 2; /data/harmony/bin/hub_sync 2>/dev/null || true; sleep 6; mount -o remount,ro /data 2>/dev/null || true; mount -o remount,ro /cache 2>/dev/null || true; /data/harmony/bin/hub_sync 2>/dev/null || true; (sleep 3 && reboot -f) >/dev/null 2>&1 &'

echo "=== Deployment to $HUB_HOST Complete! Hub is rebooting safely. ==="
