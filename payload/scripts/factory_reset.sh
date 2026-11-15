#!/bin/sh
# Harmony Hub factory reset: back to the stock Logitech firmware, with every harmony-ng file and
# secret removed. This is the Hub's only reset: the web UI / API action runs it on the Hub
# (as /data/harmony/bin/factory_reset.sh) and tools/factory_reset.py copies it to the Hub and
# runs it there.
#
# Removed: /data/harmony (devices, activities, button maps, web UI accounts and tokens, the MQTT
# configuration with its password, Bluetooth link keys and host files, all harmony-ng programs),
# the Wi-Fi configuration (back to the stock firmware's copy), SSH (server links, binary,
# authorized keys and host keys) and the web UI copy on /cache. Save a backup first
# (tools/backup_hub.py) to keep any of it.
#
# Usage: factory_reset.sh [--confirm] [--no-reboot]
#   --confirm    no prompt
#   --no-reboot  everything but the reboot (the caller reboots; tools/factory_reset.py does)

set -e

CONFIRMED=""
REBOOT=1
for _arg in "$@"; do
    case "$_arg" in
        --confirm) CONFIRMED=1 ;;
        --no-reboot) REBOOT="" ;;
        *) echo "Usage: $0 [--confirm] [--no-reboot]"; exit 2 ;;
    esac
done

if [ -z "$CONFIRMED" ]; then
    echo "================================================================="
    echo " WARNING: REVERT HARMONY HUB TO FACTORY STOCK"
    echo "================================================================="
    echo " This removes harmony-ng and restores the official Logitech firmware."
    echo " It DELETES devices, activities, button maps, accounts and tokens,"
    echo " the Wi-Fi and MQTT settings, Bluetooth keys and SSH access."
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

# / is a union of the stock read-only file system (/mnt/root) and the writable layer: a file the
# stock firmware also has goes back to the stock copy, any other is removed.
# Usage: restore_stock_file STOCK_COPY FILE
restore_stock_file() {
    if [ -f "$1" ]; then
        cp -f "$1" "$2" 2>/dev/null || true
    else
        rm -f "$2" 2>/dev/null || true
    fi
}

echo "[*] Removing the Wi-Fi configuration..."
restore_stock_file /mnt/root/etc/wpa_supplicant.conf /etc/wpa_supplicant.conf
restore_stock_file /mnt/root/etc/wpa_supplicant.ath0.conf /etc/wpa_supplicant.ath0.conf
if [ ! -f /etc/wpa_supplicant.conf ]; then
    # the stock boot script starts wpa_supplicant with this file: keep one, with no network
    printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n' > /etc/wpa_supplicant.conf 2>/dev/null || true
fi

echo "[*] Removing SSH access..."
rm -f /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp /usr/sbin/dropbearmulti \
      /data/dropbearmulti 2>/dev/null || true
rm -f /etc/dropbear/authorized_keys /etc/dropbear/dropbear_*_host_key \
      /root/.ssh/authorized_keys /home/root/.ssh/authorized_keys 2>/dev/null || true

echo "[*] Removing harmony-ng files, settings and keys..."
# the flush tool lives in /data/harmony/bin, removed below: keep a copy in RAM for the end
cp -f /data/harmony/bin/hub_util /tmp/hub_sync 2>/dev/null || true
chmod 755 /tmp/hub_sync 2>/dev/null || true
rm -f /etc/nowatchdog /etc/profile.d/00-harmony.sh /data/rf_pair_test /data/test_hub_tx /data/hub_rf 2>/dev/null || true
rm -rf /data/harmony /data/harmony-backups /data/preserved_config /cache/www /cache/icons.tar.gz \
       /cache/*.log /cache/crashlog-*.json 2>/dev/null || true

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

if [ -z "$REBOOT" ]; then
    echo "[+] Factory reset done; reboot the Hub to start the stock firmware."
    exit 0
fi
echo "[*] Rebooting into stock firmware..."
reboot -f
