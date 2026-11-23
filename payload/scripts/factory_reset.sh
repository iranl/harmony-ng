#!/bin/sh
# Harmony Hub factory reset: back to the stock Logitech firmware, with harmony-ng's data and
# secrets removed. This is the Hub's only reset: the web UI / API action runs it on the Hub
# (as /data/harmony/bin/factory_reset.sh) and tools/factory_reset.py sends it to the Hub and runs
# it there.
#
# Restored from the stock firmware (its file system mounted again read-only, as /mnt/root is
# hidden under the union): /etc/init.d/rcS.local (or the copy saved before the first install),
# /etc/network/interfaces (DHCP on Wi-Fi), the Wi-Fi configuration.
# Removed, secrets first: /data/harmony (devices, activities, button maps, web UI accounts and
# tokens, the MQTT configuration with its password, Bluetooth link keys and host files, all
# harmony-ng programs), the web UI copy on /cache, then the Wi-Fi network and SSH access (server
# links, binary, authorized keys, the host keys the installers made). A file the stock image also
# has (its RSA host key, for example) gets the stock copy back; when the stock file system cannot
# be read, such a file is left as it is.
# Kept: the stock firmware's own data (/data/resources unless it was emptied, the account ids in /data/luaworks), the
# BusyBox in /cache/bin, /cache/modules, /etc/TZ, /etc/ntp.conf, /etc/tdeenable and
# /data/stock_backup. The hardware reset (docs/FIRST_INSTALL_HUB.md) wipes everything.
# Save a backup first (tools/backup_hub.py) to keep any of it.
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

# From here on a lost terminal or SSH login does not stop the reset half way: HUP and PIPE are
# ignored and output that cannot be written is dropped.
trap '' HUP PIPE
# (in a subshell: the stock ash with set -e ends the script on a failed echo, even with || true)
say() {
    (echo "$@") 2>/dev/null || true
}

# One reset at a time: a directory in /tmp (RAM, gone at the reboot) holding the pid. One left by
# a run that no longer exists (kill -9) is taken over.
LOCK_DIR=/tmp/factory_reset.lock
LOCKED=""
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
    _other=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
    if [ -n "$_other" ] && kill -0 "$_other" 2>/dev/null; then
        say "[-] Another factory reset is running (pid $_other); nothing was changed."
        exit 1
    fi
    rm -rf "$LOCK_DIR" 2>/dev/null || true
    if ! mkdir "$LOCK_DIR" 2>/dev/null; then
        say "[-] Could not create $LOCK_DIR; nothing was changed."
        exit 1
    fi
fi
LOCKED=1
echo $$ > "$LOCK_DIR/pid" 2>/dev/null || true

say "[*] Stopping daemons..."
# Stop the init.sh supervisor loops first (as "init.sh stop" and safe_reboot.sh do): they start
# a killed daemon again after 5 s, while this script deletes its files
touch /tmp/harmony_stop 2>/dev/null || true
for _spid in $(cat /tmp/harmony_supervisors.pid 2>/dev/null); do
    kill -9 "$_spid" 2>/dev/null || true
done
rm -f /tmp/harmony_supervisors.pid 2>/dev/null || true
killall -9 init.sh hub_daemon hub_btstack hub_rf hal luaworks luadraws lua 2>/dev/null || true
# let SPI transfers in flight end before the flash writes below (as safe_reboot.sh)
sleep 2

# / is a union of the stock read-only file system and the writable layer, mounted over the
# mount points of both: /mnt/root cannot be read from the running system. The stock file system
# is mounted again, read-only, at a private directory to take its copies of the files below. It
# is unmounted at the end, on an error exit and on INT or TERM; a mount left behind by an earlier
# run (power cut, kill -9) is unmounted before.
# A file the stock firmware has is never deleted on the union (that would hide the stock copy).
STOCK_DIR=/tmp/harmony_stock_root
STOCK_MOUNTED=""
stock_umount() {
    if [ -n "$STOCK_MOUNTED" ]; then
        if umount "$STOCK_DIR" 2>/dev/null; then
            STOCK_MOUNTED=""
            rm -rf "$STOCK_DIR" 2>/dev/null || true
        fi
    fi
    return 0
}
# at the end, also on an error: unmount, then release the lock
cleanup() {
    stock_umount
    if [ -n "$LOCKED" ]; then
        rm -rf "$LOCK_DIR" 2>/dev/null || true
        LOCKED=""
    fi
    return 0
}
trap cleanup EXIT
# a deliberate stop (Ctrl-C, kill): the same, then end (the signals are ignored first: the stock
# ash runs the handler again and again otherwise)
trap 'trap "" INT TERM; cleanup >/dev/null 2>&1; exit 1' INT TERM
umount "$STOCK_DIR" 2>/dev/null || true
_stock_dev=$(awk '$2 == "/mnt/root" && $3 == "squashfs" { print $1; exit }' /proc/mounts 2>/dev/null || true)
if [ -n "$_stock_dev" ] && mkdir -p "$STOCK_DIR" 2>/dev/null &&
   mount -t squashfs -o ro "$_stock_dev" "$STOCK_DIR" 2>/dev/null; then
    STOCK_MOUNTED=1
else
    say "  -> The stock file system could not be read; stock files are only restored from saved copies"
fi

# Put the stock firmware's FILE (path inside the stock file system), or SAVED_COPY when the stock
# file system has none or could not be read, in place of DEST: written as DEST.new and renamed,
# so a failed write never leaves DEST cut. 1: no copy found, 2: the write failed (DEST unchanged).
# Usage: restore_stock FILE DEST [SAVED_COPY]
restore_stock() {
    _src=""
    if [ -n "$STOCK_MOUNTED" ] && [ -f "$STOCK_DIR/$1" ]; then
        _src="$STOCK_DIR/$1"
    elif [ -n "$3" ] && [ -f "$3" ]; then
        _src="$3"
    else
        return 1
    fi
    if cp -f "$_src" "$2.new" 2>/dev/null && mv -f "$2.new" "$2" 2>/dev/null; then
        return 0
    fi
    rm -f "$2.new" 2>/dev/null || true
    return 2
}

# A file harmony-ng added. When the stock file system has one, its copy comes back; when it has
# none, the file is removed. When the stock file system could not be read, it is removed only
# when MAYBE_STOCK is empty (a file only harmony-ng makes), else left as it is.
# Usage: drop_file FILE DEST [MAYBE_STOCK]
drop_file() {
    if [ -n "$STOCK_MOUNTED" ]; then
        if [ -e "$STOCK_DIR/$1" ]; then
            if ! { cp -f "$STOCK_DIR/$1" "$2.new" 2>/dev/null && mv -f "$2.new" "$2" 2>/dev/null; }; then
                rm -f "$2.new" 2>/dev/null || true
            fi
        else
            rm -f "$2" 2>/dev/null || true
        fi
    elif [ -z "$3" ]; then
        rm -f "$2" 2>/dev/null || true
    fi
}

say "[*] Restoring startup scripts..."
# interfaces first: harmony-ng's rcS.local edits it again at every boot, so a run cut after this
# step never leaves the stock rcS.local with harmony-ng's interfaces. harmony-ng's rcS.local took
# ath0 out of "auto" ("auto lo ath0" -> "auto lo"): the stock firmware gets its Wi-Fi address only
# through it
if ! restore_stock etc/network/interfaces /etc/network/interfaces; then
    [ -f /etc/network/interfaces ] && sed -i 's/^auto lo$/auto lo ath0/' /etc/network/interfaces 2>/dev/null || true
fi
_rcs_rc=0
restore_stock etc/init.d/rcS.local /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig || _rcs_rc=$?
if [ "$_rcs_rc" != 0 ]; then
    # nothing removed yet: start the services again instead of leaving the Hub without them (or
    # with harmony-ng's boot script, which keeps the stock UI from starting)
    if [ "$_rcs_rc" = 1 ]; then
        say "[-] No stock /etc/init.d/rcS.local found: harmony-ng stays installed, nothing was removed"
    else
        say "[-] Could not write /etc/init.d/rcS.local (flash full?): it was not changed, nothing was removed"
    fi
    say "    (only /etc/network/interfaces is back to its stock form; harmony-ng's boot script edits it"
    say "    again at the next boot). Restarting the services."
    rm -f /tmp/harmony_stop 2>/dev/null || true
    # with the default SIGHUP and SIGPIPE, as at boot (this script ignores both)
    [ -x /data/harmony/init.sh ] && (trap - HUP PIPE; /data/harmony/init.sh start >/dev/null 2>&1 &)
    exit 1
fi
chmod 755 /etc/init.d/rcS.local
say "  -> Restored the stock /etc/init.d/rcS.local"

say "[*] Restoring stock daemon permissions..."
chmod +x /usr/bin/watchdog /opt/luaworks/luaworks /usr/bin/hal /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true

say "[*] Removing harmony-ng files, settings and keys..."
# the flush tool lives in /data/harmony/bin, removed below: keep a copy in RAM for the end
cp -f /data/harmony/bin/hub_util /tmp/hub_sync 2>/dev/null || true
chmod 755 /tmp/hub_sync 2>/dev/null || true
rm -rf /data/harmony /data/harmony-backups /data/preserved_config /cache/www /cache/icons.tar.gz \
       /cache/*.log /cache/crashlog-*.json 2>/dev/null || true
rm -f /data/rf_pair_test /data/test_hub_tx /data/hub_rf 2>/dev/null || true
drop_file etc/nowatchdog /etc/nowatchdog
drop_file etc/profile.d/00-harmony.sh /etc/profile.d/00-harmony.sh

say "[*] Removing the Wi-Fi configuration..."
if ! restore_stock etc/wpa_supplicant.conf /etc/wpa_supplicant.conf; then
    # the stock boot script starts wpa_supplicant with this file: one with no network
    printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n' > /etc/wpa_supplicant.conf.new 2>/dev/null &&
        mv -f /etc/wpa_supplicant.conf.new /etc/wpa_supplicant.conf 2>/dev/null || true
fi
drop_file etc/wpa_supplicant.ath0.conf /etc/wpa_supplicant.ath0.conf

say "[*] Removing SSH access..."
# authorized_keys and the ed25519 key come from the installers; the stock image ships an RSA host
# key (and may ship the other two)
drop_file etc/dropbear/authorized_keys /etc/dropbear/authorized_keys
drop_file etc/dropbear/dropbear_ed25519_host_key /etc/dropbear/dropbear_ed25519_host_key
for _k in dropbear_rsa_host_key dropbear_dss_host_key dropbear_ecdsa_host_key; do
    drop_file "etc/dropbear/$_k" "/etc/dropbear/$_k" maybe-stock
done
drop_file root/.ssh/authorized_keys /root/.ssh/authorized_keys
drop_file home/root/.ssh/authorized_keys /home/root/.ssh/authorized_keys
for _b in dropbear dropbearkey dbclient scp dropbearmulti; do
    drop_file "usr/sbin/$_b" "/usr/sbin/$_b"
done
rm -f /data/dropbearmulti 2>/dev/null || true
stock_umount

say "[*] Syncing filesystems..."
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
    say "[+] Factory reset done; reboot the Hub to start the stock firmware."
    exit 0
fi
say "[*] Rebooting into stock firmware..."
reboot -f
