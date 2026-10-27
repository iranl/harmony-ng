#!/bin/sh
# Harmony Hub Factory Reset & Stock Revert Script
# Restores original stock scripts, permissions, and wipes custom files.

set -e

if [ "$1" != "--confirm" ]; then
    echo "================================================================="
    echo " WARNING: REVERT HARMONY HUB TO FACTORY STOCK"
    echo "================================================================="
    echo " This will remove custom software and restore official Logitech firmware."
    printf " Type 'RESET' to confirm: "
    read -r CONFIRM
    if [ "$CONFIRM" != "RESET" ]; then
        echo "Aborted."
        exit 1
    fi
fi

echo "[*] Stopping daemons..."
killall -9 init.sh hub_daemon hub_btstack hub_rf hub_dhcpd hub_portal hal luaworks luadraws lua 2>/dev/null || true

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

echo "[*] Preserving user configuration directory..."
mkdir -p /data/preserved_config 2>/dev/null || true
if [ -d /data/harmony/config ]; then
    cp -rf /data/harmony/config/* /data/preserved_config/ 2>/dev/null || true
fi
[ -f /data/harmony/mqtt_config.json ] && cp -f /data/harmony/mqtt_config.json /data/preserved_config/ 2>/dev/null || true

echo "[*] Removing custom binaries and temporary state..."
rm -f /etc/nowatchdog /usr/sbin/dropbear /usr/sbin/dropbearkey /data/rf_pair_test /data/test_hub_tx /data/hub_rf 2>/dev/null || true
rm -rf /data/harmony/bin /data/harmony/www /data/harmony-backups /cache/*.log /cache/crashlog-*.json 2>/dev/null || true
rm -f /data/harmony/*.conf /data/harmony/current_activity /data/harmony/config_version 2>/dev/null || true

echo "[*] Restoring preserved configuration for future re-install..."
mkdir -p /data/harmony/config 2>/dev/null || true
cp -rf /data/preserved_config/* /data/harmony/config/ 2>/dev/null || true
[ -f /data/preserved_config/mqtt_config.json ] && cp -f /data/preserved_config/mqtt_config.json /data/harmony/mqtt_config.json 2>/dev/null || true
rm -rf /data/preserved_config 2>/dev/null || true

echo "[*] Syncing filesystems..."
sync 2>/dev/null || true

echo "[*] Rebooting into stock firmware..."
reboot -f
