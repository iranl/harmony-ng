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
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$REPO_ROOT/payload/bin"
WWW_DIR="$REPO_ROOT/payload/www"

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

REMOTE_MD5_LIST=$(ssh -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "md5sum /data/codex/bin/* /data/codex/* /data/codex/www/* /data/codex/www/assets/* /data/codex/www/assets/icons/* /cache/bin/busybox /etc/init.d/rcS.local 2>/dev/null || true")

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
    scp -O -q -o BatchMode=yes -o StrictHostKeyChecking=no "$src" "${HUB_USER}@${HUB_HOST}:${dst}.new"
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "chmod $perm ${dst}.new 2>/dev/null && mv -f ${dst}.new $dst 2>/dev/null || true"
}

for bin in codex_daemon codex_rf codex_sntp codex_btstack codex_sync codex_dhcpd codex_portal mknod register_ehci; do
    if [ -f "$BIN_DIR/$bin" ]; then
        deploy_file_if_changed "$BIN_DIR/$bin" "/data/codex/bin/$bin" 755
    fi
done

# Extended BusyBox (default deployment)
if [ -f "$BIN_DIR/busybox" ]; then
    echo "[*] Syncing extended BusyBox to /cache/bin/..."
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "mkdir -p /cache/bin 2>/dev/null"
    deploy_file_if_changed "$BIN_DIR/busybox" "/cache/bin/busybox" 755
    ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "ln -sf /cache/bin/busybox /data/codex/bin/busybox 2>/dev/null; /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true"
fi
# Ensure root profile uses extended BusyBox in cache if present
ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" \
    "for prof in /root/.profile /home/root/.profile; do mkdir -p \"\$(dirname \"\$prof\")\" 2>/dev/null; cat <<'EOF' > \"\$prof\"
BB_PATH=\"\"
if [ -x /cache/bin/busybox ]; then
    BB_PATH=/cache/bin/busybox
elif [ -x /cache/busybox ]; then
    BB_PATH=/cache/busybox
fi
if [ -n \"\$BB_PATH\" ] && [ -z \"\$BB_EXTENDED\" ]; then
    export BB_EXTENDED=1
    export PATH=\$(dirname \"\$BB_PATH\"):\$PATH
    exec \"\$BB_PATH\" sh -l
fi
export PATH=/cache/bin:/cache:/data/codex/bin:\$PATH
EOF
chmod 644 \"\$prof\" 2>/dev/null || true; done"


# 4. Deploy WebUI to /data/codex/www/
echo "[4/5] Syncing WebUI to /data/codex/www/ (MD5 check)..."
if [ -f "$REPO_ROOT/build/gzip_assets.py" ]; then
    python3 "$REPO_ROOT/build/gzip_assets.py" "$WWW_DIR" 2>/dev/null || true
fi

# Clean up stale assets on Hub
for remote_file in $(ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "ls -1 /data/codex/www/assets/ 2>/dev/null || true"); do
    if [ ! -f "$WWW_DIR/assets/$remote_file" ]; then
        echo "    [-] Purging obsolete asset: $remote_file"
        ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "rm -f /data/codex/www/assets/$remote_file 2>/dev/null || true"
    fi
done

while IFS= read -r -d '' wfile; do
    rel="${wfile#$WWW_DIR/}"
    deploy_file_if_changed "$wfile" "/data/codex/www/$rel" 644
done < <(find "$WWW_DIR" -type f -print0)
ssh -n -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "chmod 755 /data/codex/www /data/codex/www/assets 2>/dev/null || true"

# 5. Start services
echo "[5/5] Starting codex services..."
ssh -q -o BatchMode=yes -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" \
    "killall -9 codex_daemon codex_rf 2>/dev/null || true; sleep 1; /data/codex/bin/codex_daemon >/dev/null 2>&1 & /data/codex/bin/codex_rf >/dev/null 2>&1 &"

echo "[*] Waiting 2s for daemon init..."
sleep 2

# Verification check
echo "[*] Verifying HTTP API..."
curl -s --connect-timeout 3 "http://${HUB_HOST}:8089/api/status" || echo "[!] Notice: API check returned status code or timeout"
echo ""
echo "=== Deployment to $HUB_HOST Complete! ==="
