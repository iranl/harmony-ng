#!/bin/sh
# Harmony remote factory reset: back to the stock Logitech firmware, with harmony-ng's files and
# secrets removed. This is the remote's only reset: Settings > Factory Reset runs it on the
# remote (/data/factory_reset.sh), and tools/factory_reset.py, deploy_remote.py --factory-reset
# and setup_remote.py --factory-reset copy it to the remote and run it there.
#
# Removed: the harmony-ng programs and boot script, the settings and the Hub's activity and
# device names (/data/remote_*), themes and icons, the logs, and SSH (links, binary,
# authorized keys and host keys). Kept: the stock firmware's own data (/data/resources) and the
# RF pairing with the Hub (in the radio chip). The hardware reset (Menu + Off) wipes everything.
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
    echo " WARNING: REVERT HARMONY REMOTE TO FACTORY STOCK"
    echo "================================================================="
    echo " This removes the custom remote software and restores the official Logitech firmware."
    echo " It DELETES the remote's settings, the Hub's names on it and SSH access."
    printf " Type 'RESET' to confirm: "
    read -r CONFIRM
    if [ "$CONFIRM" != "RESET" ]; then
        echo "Aborted."
        exit 1
    fi
fi

echo "[*] Stopping remote runtime..."
if [ -x /data/init.sh ]; then
    /data/init.sh stop 2>/dev/null || true
fi
killall -9 init.sh remote_ui elite_ui remote_watchdog.sh elite_watchdog.sh 2>/dev/null || true

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

echo "[*] Removing custom triggers, logs, and symlinks..."
rm -f /etc/tdeenable /etc/nowatchdog /data/reboot_counter /tmp/remote_ui.log /tmp/remote-init.log /tmp/remote_stop /tmp/remote_supervisors.pid /tmp/elite_stop /tmp/elite_supervisors.pid 2>/dev/null || true
rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true
rm -f /usr/bin/head /usr/bin/tail /usr/bin/which /usr/bin/find /usr/bin/tar /usr/bin/xargs /usr/bin/xz /usr/bin/bzcat /usr/bin/bunzip2 /usr/bin/lzma /usr/bin/unlzma /usr/bin/diff /usr/bin/patch /usr/bin/awk /usr/bin/sed 2>/dev/null || true

echo "[*] Syncing filesystems before binary removal..."
/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true
sleep 1

echo "[*] Removing custom binaries and supervisor scripts..."
rm -f /data/remote_ui /data/remote_ui.new /data/elite_ui /data/elite_ui.new /data/elite_watchdog.sh /data/init.sh /data/safe_reboot.sh /data/remote_watchdog.sh /data/factory_reset.sh /data/dropbear* /data/mknod /data/scp /data/busybox 2>/dev/null || true
rm -rf /data/stock_backup /data/assets /data/themes 2>/dev/null || true

echo "[*] Removing settings, logs and SSH keys..."
# remote_sync and the elite_sync link stay until the flush below
for _f in /data/remote_* /data/elite_* /data/codex_*; do
    case "$_f" in
        /data/remote_sync|/data/elite_sync) ;;
        *) rm -rf "$_f" 2>/dev/null || true ;;
    esac
done
rm -rf /data/remote_conf_p /data/elite_conf_p 2>/dev/null || true
# only the dropbear links of the installer live there (the stock firmware has no rmdir)
rm -rf /data/bin 2>/dev/null || true
rm -f /data/rcS.local /data/force_sync /data/flightlog_on /data/debug_logging.conf /data/power_debug.log \
      /data/crash.log /data/harmony_crash.log /data/last_kmsg.log 2>/dev/null || true
rm -f /etc/dropbear/authorized_keys /etc/dropbear/dropbear_*_host_key \
      /root/.ssh/authorized_keys /home/root/.ssh/authorized_keys 2>/dev/null || true

# Flush with our sync binary, then remove it. The stock firmware has neither a sync
# command nor a BusyBox sync applet, so the last two flush attempts below only work on a
# remote that still has one; the removal is flushed by reboot itself (BusyBox reboot
# calls sync() unless -n is given).
/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || /bin/busybox sync 2>/dev/null || sync 2>/dev/null || true
rm -f /data/remote_sync /data/elite_sync /data/sync 2>/dev/null || true
/bin/busybox sync 2>/dev/null || sync 2>/dev/null || true
sleep 1

if [ -z "$REBOOT" ]; then
    echo "[+] Factory reset done; reboot the remote to start the stock firmware."
    exit 0
fi
/sbin/reboot -f 2>/dev/null || reboot -f
