#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# If python3 is available, delegate to streaming tar sync utility
if command -v python3 >/dev/null 2>&1; then
    exec python3 "$REPO_ROOT/tools/sync_elite_to_hub.py" "$@"
fi


# Fallback bash extraction
REMOTE_HOST="${1:-192.168.2.1}"
HUB_HOST="${2:-${HUB_IP:-192.168.1.1}}"
REMOTE_USER="root"
DEST_DIR="$REPO_ROOT/payload/www/assets/icons"

mkdir -p "$DEST_DIR"

echo "=== Extracting Stock Icons from Harmony Elite Remote ==="
echo "Source: ${REMOTE_USER}@${REMOTE_HOST}:/opt/luadraws/appscommon/asset/"
echo "Destination: $DEST_DIR"

# Download stock png icons from remote firmware using -O for legacy SCP protocol
scp -O -q -o BatchMode=yes -o StrictHostKeyChecking=no -r \
    "${REMOTE_USER}@${REMOTE_HOST}:/opt/luadraws/appscommon/asset/"* "$DEST_DIR/" || {
    echo "[!] SCP failed. Ensure Harmony Elite is connected over USB at $REMOTE_HOST"
    exit 1
}

ICON_COUNT=$(find "$DEST_DIR" -type f -name "*.png" | wc -l)
echo "[+] Successfully extracted $ICON_COUNT icons to $DEST_DIR"

# If Hub IP provided, push to Hub in one go
if [ -n "$HUB_HOST" ]; then
    echo "=== Pushing Icons to Harmony Hub ($HUB_HOST) ==="
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no "${REMOTE_USER}@${HUB_HOST}" "mkdir -p /data/codex/www/assets/icons"
    scp -O -q -o BatchMode=yes -o StrictHostKeyChecking=no -r "$DEST_DIR"/* "${REMOTE_USER}@${HUB_HOST}:/data/codex/www/assets/icons/" || {
        echo "[!] SCP to Hub failed. Ensure Hub is reachable at $HUB_HOST"
        exit 1
    }
    echo "[+] Successfully pushed icons to Hub at /data/codex/www/assets/icons/"
fi
