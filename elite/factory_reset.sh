#!/bin/sh
# Harmony Elite Remote Factory Reset & Stock Revert Script
# Restores stock scripts, permissions, and wipes custom files.

set -e

if [ "$1" != "--confirm" ]; then
    echo "================================================================="
    echo " WARNING: REVERT HARMONY ELITE REMOTE TO FACTORY STOCK"
    echo "================================================================="
    echo " This will remove custom Elite software and restore official Logitech firmware."
    printf " Type 'RESET' to confirm: "
    read -r CONFIRM
    if [ "$CONFIRM" != "RESET" ]; then
        echo "Aborted."
        exit 1
    fi
fi

echo "[*] Stopping Elite runtime..."
killall -9 elite_ui elite_watchdog.sh 2>/dev/null || true

echo "[*] Restoring startup configuration..."
if [ -f /data/stock_backup/rcS.local.orig ]; then
    cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local
    chmod 755 /etc/init.d/rcS.local
    echo "  -> Restored original stock /etc/init.d/rcS.local"
else
    rm -f /etc/init.d/rcS.local
    echo "  -> Removed custom /etc/init.d/rcS.local (stock Elite boots via stock rcS)"
fi

echo "[*] Restoring stock daemon permissions..."
chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true

echo "[*] Preserving user configuration..."
mkdir -p /data/elite_conf_p 2>/dev/null || true
cp -f /data/elite_ui_config.json /data/elite_conf_p/ 2>/dev/null || true
cp -f /data/elite_ui.conf /data/elite_conf_p/ 2>/dev/null || true

echo "[*] Removing custom binaries and Dropbear symlinks..."
rm -f /etc/tdeenable /etc/nowatchdog /data/reboot_counter /tmp/elite_ui.log 2>/dev/null || true
rm -f /data/elite_ui /data/elite_ui.new /data/elite_sync /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox 2>/dev/null || true
rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true

echo "[*] Restoring preserved configuration for future re-install..."
cp -f /data/elite_conf_p/* /data/ 2>/dev/null || true
rm -rf /data/elite_conf_p 2>/dev/null || true

echo "[*] Syncing filesystems..."
/data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true

echo "[*] Rebooting into stock Logitech UI..."
reboot -f
