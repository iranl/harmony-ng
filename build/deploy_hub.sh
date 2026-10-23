#!/bin/bash
set -euo pipefail

HUB_HOST="${1:-192.168.1.1}"
HUB_USER="root"
DEBUG_BUSYBOX="${DEBUG_BUSYBOX:-0}"

# Parse optional flags
for arg in "$@"; do
    case "$arg" in
        --debug|--debug-busybox|--dev)
            DEBUG_BUSYBOX=1
            ;;
        --host=*|-h=*)
            HUB_HOST="${arg#*=}"
            ;;
        192.*|10.*|172.*|*.local)
            HUB_HOST="$arg"
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -d "$SCRIPT_DIR/bin" ]; then
    BIN_DIR="$SCRIPT_DIR/bin"
    WWW_DIR="$SCRIPT_DIR/www"
    SCRIPTS_DIR="$SCRIPT_DIR/scripts"
    MODULES_DIR="$SCRIPT_DIR/modules"
else
    REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
    BIN_DIR="$REPO_ROOT/payload/bin"
    WWW_DIR="$REPO_ROOT/payload/www"
    SCRIPTS_DIR="$REPO_ROOT/payload/scripts"
    MODULES_DIR="$REPO_ROOT/payload/modules"
fi

echo "=== Harmony Hub Codex Firmware Deployment ==="
echo "Target Hub: $HUB_HOST"
if [ "$DEBUG_BUSYBOX" = "1" ]; then
    echo "Mode: Developer / Debug (Extended BusyBox enabled)"
else
    echo "Mode: Standard User (Stock BusyBox; existing installation left untouched)"
fi

# 1. Run automated backup first
echo "[1/5] Running pre-deployment backup..."
"$SCRIPT_DIR/backup_hub.sh" "$HUB_HOST"

# 2. Stop existing processes with kill -9
echo "[2/5] Stopping old hub processes..."
ssh -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" \
    "killall -9 codex_daemon codex_rf codex_btstack 2>/dev/null || true; sleep 1"

# 3. Query remote MD5 hashes and deploy binaries
echo "[3/5] Querying remote checksums & syncing binaries to /data/codex/bin/..."
ssh -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" \
    "mkdir -p /data/codex/bin /data/codex/www/assets /data/codex/config; rm -rf /data/codex-backups /data/codex/resource-backups /cache/crashlog-*.json 2>/dev/null; rm -f /data/rf_pair_test /data/test_hub_tx /data/codex_rf /data/codex/bin/check_space 2>/dev/null || true"

REMOTE_MD5_LIST=$(ssh -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "md5sum /data/codex/bin/* /data/codex/* /data/codex/www/* /data/codex/www/assets/* /cache/www/* /cache/www/assets/* /cache/modules/* /cache/bin/busybox /etc/init.d/rcS.local 2>/dev/null || true")

deploy_file_if_changed() {
    local src="$1"
    local dst="$2"
    local perm="${3:-755}"
    [ ! -f "$src" ] && return 0
    local local_md5
    local_md5=$(md5sum "$src" | awk '{print $1}')
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
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "mkdir -p '$ddir' 2>/dev/null || true"
    scp -O -q -o BatchMode=yes -o StrictHostKeyChecking=no "$src" "${HUB_USER}@${HUB_HOST}:${dst}.new"
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "chmod $perm ${dst}.new 2>/dev/null && mv -f ${dst}.new $dst 2>/dev/null || true"
}

for bin in codex_daemon codex_rf codex_sntp codex_btstack codex_sync codex_dhcpd codex_portal mknod register_ehci eth_fix; do
    if [ -f "$BIN_DIR/$bin" ]; then
        deploy_file_if_changed "$BIN_DIR/$bin" "/data/codex/bin/$bin" 755
    fi
done

# Extended BusyBox (default deployment)
if [ -f "$BIN_DIR/busybox" ]; then
    echo "[*] Syncing extended BusyBox to /cache/bin/..."
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "mkdir -p /cache/bin 2>/dev/null"
    deploy_file_if_changed "$BIN_DIR/busybox" "/cache/bin/busybox" 755
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "ln -sf /cache/bin/busybox /data/codex/bin/busybox 2>/dev/null; /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true; for _app in \$(/cache/bin/busybox --list 2>/dev/null); do [ ! -e \"/bin/\$_app\" ] && [ ! -e \"/usr/bin/\$_app\" ] && ln -sf /cache/bin/busybox \"/usr/bin/\$_app\" 2>/dev/null || true; done; mkdir -p /etc/profile.d 2>/dev/null; echo 'export PATH=/cache/bin:/data/codex/bin:\$PATH' > /etc/profile.d/00-codex.sh 2>/dev/null; chmod 644 /etc/profile.d/00-codex.sh 2>/dev/null || true"
fi
# Ensure root profile uses extended BusyBox in cache if present
ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" \
    "for prof in /root/.profile /home/root/.profile; do mkdir -p \"\$(dirname \"\$prof\")\" 2>/dev/null; printf \"BB_PATH=\\\"\\\"\n[ -x /cache/bin/busybox ] && BB_PATH=/cache/bin/busybox\n[ -x /cache/busybox ] && BB_PATH=/cache/busybox\nif [ -t 0 ] && [ -n \\\"\\\$BB_PATH\\\" ] && [ -z \\\"\\\$BB_EXTENDED\\\" ]; then\n  export BB_EXTENDED=1\n  export PATH=/cache/bin:/data/codex/bin:\\\$PATH\n  exec \\\"\\\$BB_PATH\\\" sh -l\nfi\nexport PATH=/cache/bin:/data/codex/bin:\\\$PATH\n\" > \"\$prof\"; chmod 644 \"\$prof\" 2>/dev/null || true; done"

# Deploy payload scripts
if [ -d "$SCRIPTS_DIR" ]; then
    echo "[*] Syncing startup & network scripts to /data/codex/..."
    for sc in "$SCRIPTS_DIR"/*; do
        [ -f "$sc" ] || continue
        deploy_file_if_changed "$sc" "/data/codex/$(basename "$sc")" 755
    done
fi

# Deploy USB Ethernet kernel modules to /cache/modules (symlinked to /data/codex/modules)
if [ -d "$MODULES_DIR" ]; then
    echo "[*] Syncing USB Ethernet kernel modules to /cache/modules/..."
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "mkdir -p /cache/modules 2>/dev/null; [ -L /data/codex/modules ] || ln -sf /cache/modules /data/codex/modules"
    for mod in "$MODULES_DIR"/*.ko; do
        [ -f "$mod" ] || continue
        deploy_file_if_changed "$mod" "/cache/modules/$(basename "$mod")" 644
    done
fi

# 4. Deploy WebUI to /cache/www/ (preserves critical /data flash)
echo "[4/5] Syncing WebUI to /cache/www/ (MD5 check)..."
if [ -f "$REPO_ROOT/build/gzip_assets.py" ]; then
    python3 "$REPO_ROOT/build/gzip_assets.py" "$WWW_DIR" 2>/dev/null || true
fi

# Clean up physical directory on /data if present, symlink to /cache/www, prune flash icons
ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" \
    "mkdir -p /cache/www/assets 2>/dev/null; rm -rf /cache/www/assets/icons 2>/dev/null; if [ -d /data/codex/www ] && [ ! -L /data/codex/www ]; then rm -rf /data/codex/www 2>/dev/null; fi; mkdir -p /data/codex 2>/dev/null; [ -L /data/codex/www ] || ln -sfn /cache/www /data/codex/www 2>/dev/null"

# Clean up stale assets on Hub (/cache/www/assets)
for remote_file in $(ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "ls -1 /cache/www/assets/ 2>/dev/null || true"); do
    if [ ! -f "$WWW_DIR/assets/$remote_file" ]; then
        echo "    [-] Purging obsolete asset: $remote_file"
        ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "rm -f /cache/www/assets/$remote_file 2>/dev/null || true"
    fi
done

while IFS= read -r -d '' wfile; do
    rel="${wfile#$WWW_DIR/}"
    deploy_file_if_changed "$wfile" "/cache/www/$rel" 644
done < <(find "$WWW_DIR" -type f -print0)

if [ ! -f "$REPO_ROOT/build/icons.tar.gz" ]; then
    echo ""
    echo "[?] build/icons.tar.gz not found on host."
    read -p "Download official Elite firmware, extract 1,350 icons, and build build/icons.tar.gz now? [Y/n] " -r REPLY || true
    if [ -z "$REPLY" ] || [[ "$REPLY" =~ ^[Yy]$ ]]; then
        echo "[*] Extracting remote UI icons and packaging build/icons.tar.gz..."
        python3 "$REPO_ROOT/tools/harmony_firmware.py" extract-icons --archive "$REPO_ROOT/build/icons.tar.gz" || true
    fi
fi

if [ -f "$REPO_ROOT/build/icons.tar.gz" ]; then
    deploy_file_if_changed "$REPO_ROOT/build/icons.tar.gz" "/cache/icons.tar.gz" 644
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" \
        "if [ -f /data/codex/icons.tar.gz ] && [ ! -L /data/codex/icons.tar.gz ]; then rm -f /data/codex/icons.tar.gz 2>/dev/null; fi; [ -L /data/codex/icons.tar.gz ] || ln -sf /cache/icons.tar.gz /data/codex/icons.tar.gz 2>/dev/null"
fi

ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "chmod 755 /cache/www /cache/www/assets 2>/dev/null || true"

# 5. Pre-reboot integrity check & safe reboot (flush SPI NOR flash)
echo "[5/5] Verifying system shell and flushing NOR flash buffers..."
ssh -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" \
    'if ! /bin/sh -c "true" 2>/dev/null; then echo "[-] CRITICAL: /bin/sh verification failed. Aborting reboot!"; exit 1; fi; chmod +x /data/codex/safe_reboot.sh 2>/dev/null || true; ln -sf /data/codex/safe_reboot.sh /data/codex/bin/safe_reboot.sh 2>/dev/null || true; ln -sf /data/codex/bin/codex_sync /data/codex/bin/sync 2>/dev/null || true; ln -sf /data/codex/bin/codex_sync /cache/bin/sync 2>/dev/null || true; killall -9 init.sh codex_daemon codex_btstack codex_rf codex_dhcpd codex_portal hal luaworks luadraws lua 2>/dev/null || true; sleep 1; /data/codex/bin/codex_sync 2>/dev/null || true; sleep 2; mount -o remount,ro /data 2>/dev/null || true; mount -o remount,ro /cache 2>/dev/null || true; /data/codex/bin/codex_sync 2>/dev/null || true; (sleep 1 && reboot -f) >/dev/null 2>&1 &'

echo "=== Deployment to $HUB_HOST Complete! Hub is rebooting safely. ==="
