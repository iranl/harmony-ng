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
if [ -x /data/init.sh ]; then
    /data/init.sh stop 2>/dev/null || true
fi
killall -9 init.sh elite_ui elite_watchdog.sh 2>/dev/null || true

echo "[*] Restoring startup configuration..."
if [ -f /data/stock_backup/rcS.local.orig ] && ! grep -q 'usbeth' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'dropbear' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'exit 0' /data/stock_backup/rcS.local.orig 2>/dev/null; then
    cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local
    chmod 755 /etc/init.d/rcS.local
    echo "  -> Restored original stock /etc/init.d/rcS.local"
else
    cat << 'EOF' > /etc/init.d/rcS.local
#!/bin/sh
/usr/bin/flashuboot /lib/firmware/u-boot.bin 2>/dev/null || true
FW_VERSION=$(cat /lib/firmware/msp430.version 2>/dev/null || true)
VERSION=$(cat /sys/bus/i2c/devices/1-0034/version 2>/dev/null || true)
if [ "$FW_VERSION" != "$VERSION" ] && [ -f /lib/firmware/msp430.txt ]; then
	cat /lib/firmware/msp430.txt > /dev/mspfw 2>/dev/null || true
fi
VERSION=$(cat /sys/bus/i2c/devices/1-0020/version 2>/dev/null || true)
if [ "$VERSION" = "TM2176-003-0" ] && [ -f /lib/firmware/tm2176-003.img ]; then
	cat /lib/firmware/tm2176-003.img > /dev/clearpad 2>/dev/null || true
fi
EOF
    chmod 755 /etc/init.d/rcS.local
    echo "  -> Restored clean stock /etc/init.d/rcS.local"
fi

echo "[*] Restoring stock daemon permissions..."
chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true

echo "[*] Preserving user configuration..."
mkdir -p /data/elite_conf_p 2>/dev/null || true
cp -f /data/elite_rf_config.json /data/elite_conf_p/ 2>/dev/null || true
cp -f /data/elite_settings.conf /data/elite_conf_p/ 2>/dev/null || true
cp -f /data/elite_ui.conf /data/elite_conf_p/ 2>/dev/null || true

echo "[*] Removing custom triggers, logs, and symlinks..."
rm -f /etc/tdeenable /etc/nowatchdog /data/reboot_counter /tmp/elite_ui.log /tmp/elite-init.log /tmp/elite_stop /tmp/elite_supervisors.pid 2>/dev/null || true
rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true
rm -f /usr/bin/head /usr/bin/tail /usr/bin/which /usr/bin/find /usr/bin/tar /usr/bin/xargs /usr/bin/xz /usr/bin/bzcat /usr/bin/bunzip2 /usr/bin/lzma /usr/bin/unlzma /usr/bin/diff /usr/bin/patch /usr/bin/awk /usr/bin/sed 2>/dev/null || true

echo "[*] Syncing filesystems before binary removal..."
/data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true
sleep 1

echo "[*] Removing custom binaries and supervisor scripts..."
rm -f /data/elite_ui /data/elite_ui.new /data/elite_sync /data/init.sh /data/safe_reboot.sh /data/elite_watchdog.sh /data/factory_reset.sh /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox 2>/dev/null || true
rm -rf /data/stock_backup /data/assets 2>/dev/null || true

echo "[*] Restoring preserved configuration for future re-install..."
cp -f /data/elite_conf_p/* /data/ 2>/dev/null || true
rm -rf /data/elite_conf_p 2>/dev/null || true

/data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true
sleep 1

/sbin/reboot -f 2>/dev/null || reboot -f
