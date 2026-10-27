#!/bin/bash
set -euo pipefail

HUB_HOST="${1:-192.168.1.1}"
HUB_USER="root"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
if [ -d "$SCRIPT_DIR/bin" ]; then
    BACKUP_DIR="$SCRIPT_DIR/backups"  # release bundle: same backups/ as tools/root_hub_usb.py
else
    BACKUP_DIR="$REPO_ROOT/backups"
fi
GOLDEN_FILE="$BACKUP_DIR/golden_hub_backup.tar"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$BACKUP_DIR"

# SSH key: $SSH_KEY, else the newest ~/.ssh/harmony_owner_* (authorized by root_hub_usb.py), id_rsa, id_ed25519
SSH_KEY="${SSH_KEY:-}"
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

echo "[*] Connecting to Harmony Hub ($HUB_HOST)..."

# Detect tar binary on Hub (stock BusyBox lacks tar)
TAR_CMD="tar"
if ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "test -x /cache/bin/busybox" 2>/dev/null; then
    TAR_CMD="/cache/bin/busybox tar"
elif ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "test -x /data/harmony/bin/busybox" 2>/dev/null; then
    TAR_CMD="/data/harmony/bin/busybox tar"
fi

if ! ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" "$TAR_CMD --help >/dev/null 2>&1"; then
    echo "[!] Notice: tar applet not found on Hub BusyBox. Skipping pre-deployment backup."
    exit 0
fi

# 1. Golden backup: First ever backup kept permanently for pristine restore (plain tar, no gzip)
if [ ! -f "$GOLDEN_FILE" ]; then
    echo "[*] Creating GOLDEN initial backup of Hub stock state..."
    ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
        "$TAR_CMD -cf - /data/resources /etc/passwd /etc/network /etc/TZ 2>/dev/null || true" > "$GOLDEN_FILE" || {
        echo "[!] Golden backup failed to download"
        rm -f "$GOLDEN_FILE"
    }
    if [ -f "$GOLDEN_FILE" ] && [ -s "$GOLDEN_FILE" ]; then
        echo "[+] Golden backup saved to $GOLDEN_FILE ($(du -h "$GOLDEN_FILE" | cut -f1))"
    fi
else
    echo "[+] Golden backup already exists: $GOLDEN_FILE"
fi

# 2. Rolling backup: includes /data/harmony and configs (plain tar, no gzip)
NEW_BACKUP="$BACKUP_DIR/hub_backup_${TIMESTAMP}.tar"
echo "[*] Creating rolling backup..."
ssh "${SSH_OPTS[@]}" "${HUB_USER}@${HUB_HOST}" \
    "$TAR_CMD -cf - /data/harmony /data/resources /etc/passwd /etc/network /etc/TZ 2>/dev/null || true" > "$NEW_BACKUP" || {
    echo "[!] Rolling backup failed"
    rm -f "$NEW_BACKUP"
    exit 1
}

if [ -f "$NEW_BACKUP" ] && [ -s "$NEW_BACKUP" ]; then
    echo "[+] Rolling backup saved: $NEW_BACKUP ($(du -h "$NEW_BACKUP" | cut -f1))"
fi

# 3. Rotate: Retain only 5 newest rolling backups
echo "[*] Rotating backups (keeping newest 5)..."
find "$BACKUP_DIR" -maxdepth 1 -name "hub_backup_*.tar" -type f | sort -r | sed -e '1,5d' | xargs -r rm -f

echo "[+] Active backups in $BACKUP_DIR:"
ls -lh "$BACKUP_DIR"
