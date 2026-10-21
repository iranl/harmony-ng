#!/bin/sh
# Harmony Hub Factory Reset & Stock Revert Script
# Restores original stock scripts, permissions, and wipes Codex files.

set -e

if [ "$1" != "--confirm" ]; then
    echo "================================================================="
    echo " WARNING: REVERT HARMONY HUB TO FACTORY STOCK"
    echo "================================================================="
    echo " This will remove all Codex software and restore official Logitech firmware."
    printf " Type 'RESET' to confirm: "
    read -r CONFIRM
    if [ "$CONFIRM" != "RESET" ]; then
        echo "Aborted."
        exit 1
    fi
fi

echo "[*] Stopping Codex daemons..."
killall -9 init.sh codex_daemon codex_btstack codex_rf codex_dhcpd codex_portal hal luaworks luadraws lua 2>/dev/null || true

echo "[*] Restoring startup scripts..."
if [ -f /data/stock_backup/rcS.local.orig ]; then
    cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local
    chmod 755 /etc/init.d/rcS.local
    echo "  -> Restored original stock /etc/init.d/rcS.local"
else
    rm -f /etc/init.d/rcS.local
    echo "  -> Removed custom /etc/init.d/rcS.local"
fi

echo "[*] Restoring stock daemon permissions..."
chmod +x /usr/bin/watchdog /opt/luaworks/luaworks /usr/bin/hal /usr/sbin/bluetoothd 2>/dev/null || true

echo "[*] Preserving user configuration directory (/data/codex/config)..."
mkdir -p /data/codex_preserved_config 2>/dev/null || true
cp -rf /data/codex/config/* /data/codex_preserved_config/ 2>/dev/null || true

echo "[*] Removing Codex binaries and temporary state..."
rm -f /etc/nowatchdog /usr/sbin/dropbear /usr/sbin/dropbearkey /data/rf_pair_test /data/test_hub_tx /data/codex_rf 2>/dev/null || true
rm -rf /data/codex/bin /data/codex/www /data/codex-backups /cache/*.log /cache/crashlog-*.json 2>/dev/null || true
rm -f /data/codex/*.conf /data/codex/current_activity /data/codex/config_version 2>/dev/null || true


echo "[*] Restoring preserved configuration for future re-install..."
mkdir -p /data/codex/config 2>/dev/null || true
cp -rf /data/codex_preserved_config/* /data/codex/config/ 2>/dev/null || true
rm -rf /data/codex_preserved_config 2>/dev/null || true

echo "[*] Syncing filesystems..."
sync 2>/dev/null || true

echo "[*] Rebooting into stock firmware..."
reboot -f
