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
# Stop the init.sh supervisor loops first (as "init.sh stop" and safe_reboot.sh do): they start
# a killed daemon again after 5 s, while this script deletes its files
touch /tmp/harmony_stop 2>/dev/null || true
for _spid in $(cat /tmp/harmony_supervisors.pid 2>/dev/null); do
    kill -9 "$_spid" 2>/dev/null || true
done
rm -f /tmp/harmony_supervisors.pid 2>/dev/null || true
killall -9 init.sh hub_daemon hub_btstack hub_rf hub_dhcpd hub_portal hal luaworks luadraws lua 2>/dev/null || true
# let SPI transfers in flight end before the flash writes below (as safe_reboot.sh)
sleep 2

echo "[*] Restoring startup scripts..."
if [ -f /data/stock_backup/rcS.local.orig ]; then
    if ! cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; then
        # nothing removed yet: start the services again instead of leaving the Hub without them
        echo "[-] Could not restore /etc/init.d/rcS.local; nothing was removed. Restarting the services."
        rm -f /tmp/harmony_stop 2>/dev/null || true
        [ -x /data/harmony/init.sh ] && /data/harmony/init.sh start >/dev/null 2>&1 &
        exit 1
    fi
    chmod 755 /etc/init.d/rcS.local
    echo "  -> Restored original stock /etc/init.d/rcS.local"
else
    rm -f /etc/init.d/rcS.local
    echo "  -> Removed custom /etc/init.d/rcS.local"
fi

echo "[*] Restoring stock daemon permissions..."
chmod +x /usr/bin/watchdog /opt/luaworks/luaworks /usr/bin/hal /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true

echo "[*] Preserving user configuration directory..."
mkdir -p /data/preserved_config 2>/dev/null || true
if [ -d /data/harmony/config ]; then
    cp -rf /data/harmony/config/* /data/preserved_config/ 2>/dev/null || true
fi
[ -f /data/harmony/mqtt.json ] && cp -f /data/harmony/mqtt.json /data/preserved_config/ 2>/dev/null || true

echo "[*] Removing custom binaries and temporary state..."
# the flush tool lives in /data/harmony/bin, removed below: keep a copy in RAM for the end
cp -f /data/harmony/bin/hub_util /tmp/hub_sync 2>/dev/null || true
chmod 755 /tmp/hub_sync 2>/dev/null || true
rm -f /etc/nowatchdog /usr/sbin/dropbear /usr/sbin/dropbearkey /data/rf_pair_test /data/test_hub_tx /data/hub_rf 2>/dev/null || true
rm -rf /data/harmony/bin /data/harmony/www /data/harmony-backups /cache/*.log /cache/crashlog-*.json 2>/dev/null || true
rm -f /data/harmony/*.conf /data/harmony/current_activity /data/harmony/config_version 2>/dev/null || true

echo "[*] Restoring preserved configuration for future re-install..."
mkdir -p /data/harmony/config 2>/dev/null || true
cp -rf /data/preserved_config/* /data/harmony/config/ 2>/dev/null || true
[ -f /data/preserved_config/mqtt.json ] && cp -f /data/preserved_config/mqtt.json /data/harmony/mqtt.json 2>/dev/null || true
rm -rf /data/preserved_config 2>/dev/null || true

echo "[*] Syncing filesystems..."
# the Hub has no plain "sync": hub_sync (hub_util run under that name), then BusyBox
flush_fs() {
    /tmp/hub_sync 2>/dev/null || /cache/bin/busybox sync 2>/dev/null || sync 2>/dev/null || true
}
flush_fs
sleep 2
# read-only remount commits clean file system state before the reset (as safe_reboot.sh)
mount -o remount,ro /data 2>/dev/null || true
mount -o remount,ro /cache 2>/dev/null || true
flush_fs

echo "[*] Rebooting into stock firmware..."
reboot -f
