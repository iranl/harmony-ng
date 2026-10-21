#!/bin/sh
# Harmony Elite Remote Factory Reset & Stock Revert Script
# Restores stock scripts, permissions, and wipes custom Codex files.

set -e

if [ "$1" != "--confirm" ]; then
    echo "================================================================="
    echo " WARNING: REVERT HARMONY ELITE REMOTE TO FACTORY STOCK"
    echo "================================================================="
    echo " This will remove Codex Elite and restore official Logitech firmware."
    printf " Type 'RESET' to confirm: "
    read -r CONFIRM
    if [ "$CONFIRM" != "RESET" ]; then
        echo "Aborted."
        exit 1
    fi
fi

echo "[*] Stopping codex_elite..."
killall -9 codex_elite 2>/dev/null || true

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

echo "[*] Preserving user configuration (/data/codex_elite_config.json)..."
mkdir -p /data/elite_conf_p 2>/dev/null || true
cp -f /data/codex_elite_config.json /data/elite_conf_p/ 2>/dev/null || true
cp -f /data/codex_elite.conf /data/elite_conf_p/ 2>/dev/null || true

echo "[*] Removing Codex binaries and Dropbear symlinks..."
rm -f /etc/tdeenable /etc/nowatchdog /data/reboot_counter /tmp/codex_elite.log 2>/dev/null || true
rm -f /data/codex_elite /data/codex_elite.new /data/codex_sync /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox 2>/dev/null || true
rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true

echo "[*] Restoring preserved configuration for future re-install..."
cp -f /data/elite_conf_p/codex_elite_config.json /data/ 2>/dev/null || true
cp -f /data/elite_conf_p/codex_elite.conf /data/ 2>/dev/null || true
rm -rf /data/elite_conf_p 2>/dev/null || true

echo "[*] Syncing filesystems..."
/data/codex_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true

echo "[*] Rebooting into stock Logitech UI..."
reboot -f
