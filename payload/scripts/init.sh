#!/bin/sh
PATH=/data/harmony/bin:/cache/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
trap '' HUP
LOG=/tmp/harmony-init.log

sync_fs() {
  /data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true
}

echo "$(date) harmony init start: $1" >> "$LOG"

case "$1" in
  stop)
    echo "$(date) Stopping harmony services..." >> "$LOG"
    killall -9 hub_daemon hub_rf hub_btstack 2>/dev/null || true
    rm -f /tmp/harmony_watchdog.pid 2>/dev/null || true
    exit 0
    ;;
  restart)
    echo "$(date) Restarting harmony services..." >> "$LOG"
    killall -9 hub_daemon hub_rf hub_btstack 2>/dev/null || true
    sleep 2
    # If supervisor loops already running from boot, let them respawn daemons
    if [ -f /tmp/harmony_watchdog.pid ]; then
      _old=$(cat /tmp/harmony_watchdog.pid 2>/dev/null)
      if [ -n "$_old" ] && [ -d "/proc/$_old" ]; then
        exit 0
      fi
    fi
    ;;
esac

# Avoid duplicate supervisor loop spawns
if [ -f /tmp/harmony_watchdog.pid ]; then
  _old=$(cat /tmp/harmony_watchdog.pid 2>/dev/null)
  if [ -n "$_old" ] && [ -d "/proc/$_old" ] && [ "$_old" != "$$" ]; then
    echo "$(date) harmony supervisor already running (PID $_old)" >> "$LOG"
    exit 0
  fi
fi
echo "$$" > /tmp/harmony_watchdog.pid

# 5-minute stability timer in background
(
  sleep 300
  echo 0 > /data/harmony/reboot_counter 2>/dev/null || true
  sync_fs
  echo "$(date) Hub uptime > 5m achieved, reboot counter reset" >> /tmp/boot_stability.log
) &

# 0. Neuter watchdog and crash reporting immediately
touch /etc/nowatchdog 2>/dev/null || true
chmod -x /usr/bin/watchdog 2>/dev/null || true
killall -9 watchdog 2>/dev/null || true

export LD_PRELOAD=""

# Set kernel printk loglevel according to debug logging config
if [ -f /data/harmony/debug_logging.conf ]; then
  echo "7 4 1 7" > /proc/sys/kernel/printk 2>/dev/null || true
else
  echo "3 4 1 7" > /proc/sys/kernel/printk 2>/dev/null || true
fi

# 1. Clean up legacy flash artifacts to minimize flash wear and reclaim space
rm -rf /cache/*.log /cache/crashlog-*.json /tmp/harmony_bthid_remote.pid 2>/dev/null || true
rm -rf /cache/www/assets/icons 2>/dev/null || true
for _f in /cache/www/assets/*.js /cache/www/assets/*.css /cache/www/index.html; do
  [ -f "${_f}.gz" ] && rm -f "$_f" 2>/dev/null || true
done
rm -f /cache/www/assets/*.png.gz /cache/www/*.png.gz 2>/dev/null || true
rm -f /data/*.ota* /data/*.tar.gz /data/*.zip /data/harmony/*.log /data/harmony/bin/*.old /data/harmony/bin/*.bak 2>/dev/null || true
rm -f /data/harmony/bin/pair_b25.sh /data/harmony/bin/do_pair.sh /data/harmony/bin/test_ble_diag /data/harmony/bin/test_smp /data/harmony/bin/test_hci_sniff /data/rf_pair_test /data/test_hub_tx 2>/dev/null || true
rm -f /data/harmony/g_serial.ko /data/harmony/mknod_bin /data/harmony/mknod 2>/dev/null || true
rm -f /data/harmony/modules/*.bak 2>/dev/null || true

# Maintain WebUI in /cache/www to preserve critical /data flash space
if [ -d /data/harmony/www ] && [ ! -L /data/harmony/www ]; then
  mkdir -p /cache/www 2>/dev/null
  cp -rf /data/harmony/www/* /cache/www/ 2>/dev/null || true
  rm -rf /data/harmony/www 2>/dev/null || true
fi
mkdir -p /cache/www/assets/icons 2>/dev/null || true
[ -L /data/harmony/www ] || ln -sfn /cache/www /data/harmony/www 2>/dev/null || true

# Prune unreferenced/orphaned web UI bundles in /cache/www/assets
if [ -f /cache/www/index.html ] && [ -d /cache/www/assets ]; then
  _ajs=$(sed -n 's/.*src=["'\'']\.\/assets\/\(index-[^"'\'']*\.js\)["'\''].*/\1/p' /cache/www/index.html 2>/dev/null)
  _acss=$(sed -n 's/.*href=["'\'']\.\/assets\/\(index-[^"'\'']*\.css\)["'\''].*/\1/p' /cache/www/index.html 2>/dev/null)
  if [ -n "$_ajs" ] && [ -n "$_acss" ]; then
    _ajsb="${_ajs%.js}"
    _acssb="${_acss%.css}"
    for _f in /cache/www/assets/index-*.js* /cache/www/assets/index-*.css*; do
      [ -f "$_f" ] || continue
      _fn="${_f##*/}"
      case "$_fn" in
        "${_ajsb}"*|"${_acssb}"*) ;;
        *) rm -f "$_f" ;;
      esac
    done
  fi
fi

# 2. Disable and kill unneeded stock Logitech runtime (frees ~12MB RAM)
chmod -x /opt/luaworks/luaworks 2>/dev/null || true
(
  for i in 1 2 3 4 5; do
    killall -9 luaworks luadraws lua netmonitor 2>/dev/null || true
    sleep 1
  done
) &

# 3. Shell Profile & Extended BusyBox (Installed strictly into /cache/bin; /bin/sh is NEVER touched)
rm -f /data/bin/sh /data/bin/ash /data/bin/busybox 2>/dev/null || true
mkdir -p /data/harmony/bin /data/harmony/config 2>/dev/null || true
if [ -x /cache/bin/busybox ]; then
  /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true
  ln -sf /cache/bin/busybox /data/harmony/bin/busybox 2>/dev/null || true
fi
ln -sf /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true
ln -sf /data/harmony/bin/hub_sync /cache/bin/sync 2>/dev/null || true

[ -f /data/harmony/bin/safe_reboot.sh ] && chmod +x /data/harmony/bin/safe_reboot.sh 2>/dev/null || true
[ -f /data/harmony/safe_reboot.sh ] && ln -sf /data/harmony/safe_reboot.sh /data/harmony/bin/safe_reboot.sh 2>/dev/null || true

mkdir -p /etc/profile.d 2>/dev/null || true
cat <<'EOF' > /etc/profile.d/00-harmony.sh
export PATH=/data/harmony/bin:/cache/bin:$PATH
EOF
chmod 644 /etc/profile.d/00-harmony.sh 2>/dev/null || true

for prof in /root/.profile /home/root/.profile; do
  mkdir -p "$(dirname "$prof")" 2>/dev/null || true
  cat <<'EOF' > "$prof"
BB_PATH=""
if [ -x /cache/bin/busybox ]; then
    BB_PATH=/cache/bin/busybox
elif [ -x /cache/busybox ]; then
    BB_PATH=/cache/busybox
fi
if [ -t 0 ] && [ -n "$BB_PATH" ] && [ -z "$BB_EXTENDED" ]; then
    export BB_EXTENDED=1
    export PATH=/data/harmony/bin:/cache/bin:$PATH
    exec "$BB_PATH" sh -l
fi
export PATH=/data/harmony/bin:/cache/bin:$PATH
EOF
  chmod 644 "$prof" 2>/dev/null || true
done

_dropbear_bin=""
[ -x /data/harmony/bin/dropbear ] && _dropbear_bin=/data/harmony/bin/dropbear
if [ -n "$_dropbear_bin" ]; then
  cat <<EOF > /usr/sbin/dropbear
#!/bin/sh
export PATH=/data/harmony/bin:/cache/bin:\$PATH
exec "$_dropbear_bin" -K 300 "\$@"
EOF
  chmod 755 /usr/sbin/dropbear
  if ! ps | grep '[d]ropbear' >/dev/null 2>&1; then
    /usr/sbin/dropbear
  fi
fi

# 4. Disable and kill Logitech HAL & D-Bus (direct /dev/i2s IR active, saves ~4.5MB RAM)
chmod -x /usr/bin/hal /usr/bin/dbus-daemon 2>/dev/null || true
killall -9 hal dbus-daemon 2>/dev/null || true

# 5. Network Time Sync (SNTP)
_sntp_bin=""
[ -x /data/harmony/bin/hub_sntp ] && _sntp_bin=/data/harmony/bin/hub_sntp
if [ -n "$_sntp_bin" ]; then
  (
    for i in 1 2 3 4 5 6 7 8 9 10; do
      if "$_sntp_bin" pool.ntp.org --set >> "$LOG" 2>&1; then
        echo "$(date) NTP time sync succeeded" >> "$LOG"
        break
      fi
      sleep 3
    done
  ) &
fi

# 5.2 Network Manager (USB Gadget 15s check, Ethernet startup & Wi-Fi fallback)
_net_mgr=""
[ -x /data/harmony/network_manager.sh ] && _net_mgr=/data/harmony/network_manager.sh
[ -z "$_net_mgr" ] && [ -x /data/harmony/bin/network_manager.sh ] && _net_mgr=/data/harmony/bin/network_manager.sh
if [ -n "$_net_mgr" ]; then
  "$_net_mgr" boot >> "$LOG" 2>&1 &
fi

# 6. Unified Hub Daemon (WebUI & API on 8089)
(
  _delay=5
  while true; do
    _daemon=""
    [ -x /data/harmony/bin/hub_daemon ] && _daemon=/data/harmony/bin/hub_daemon
    if [ -n "$_daemon" ]; then
      if ! pidof hub_daemon >/dev/null 2>&1; then
        echo "$(date) Starting hub_daemon..." >> "$LOG"
        "$_daemon" 8089 >> "$LOG" 2>&1
        echo "$(date) hub_daemon exited, retry in ${_delay}s" >> "$LOG"
        sleep "$_delay"
        [ "$_delay" -lt 60 ] && _delay=$(expr "$_delay" + "$_delay")
        continue
      fi
    fi
    _delay=5
    sleep 5
  done
) &

# 7. BTstack Bluetooth Engine (BlueZ 4 permanently disabled)
chmod -x /usr/sbin/bluetoothd 2>/dev/null || true
killall -9 bluetoothd 2>/dev/null || true
(
  _delay=5
  while true; do
    if [ -f /data/harmony/bt_disabled.conf ]; then
      if pidof hub_btstack >/dev/null 2>&1; then
        echo "$(date) BT disabled flag detected, stopping BTstack..." >> "$LOG"
        killall -9 hub_btstack 2>/dev/null || true
        hciconfig hci0 down 2>/dev/null || true
      fi
      sleep 5
      continue
    fi

    # Ensure hci0 is UP
    if ! hciconfig hci0 2>/dev/null | grep -q 'UP'; then
      hciconfig hci0 up 2>/dev/null || true
      sleep 1
    fi

    _bt_bin=""
    [ -x /data/harmony/bin/hub_btstack ] && _bt_bin=/data/harmony/bin/hub_btstack
    if [ -n "$_bt_bin" ]; then
      if ! pidof hub_btstack >/dev/null 2>&1; then
        echo "$(date) Starting BTstack backend..." >> "$LOG"
        "$_bt_bin" >> "$LOG" 2>&1
        echo "$(date) BTstack exited, retry in ${_delay}s" >> "$LOG"
        sleep "$_delay"
        [ "$_delay" -lt 60 ] && _delay=$(expr "$_delay" + "$_delay")
        continue
      fi
    fi
    _delay=5
    sleep 5
  done
) &

# 8. CC2544 RF Daemon (Logitech HAL replacement for Elite Remote RF)
(
  _delay=5
  while true; do
    if [ -f /data/harmony/rf_disabled.conf ]; then
      if pidof hub_rf >/dev/null 2>&1; then
        echo "$(date) RF disabled flag detected, stopping RF..." >> "$LOG"
        killall -9 hub_rf 2>/dev/null || true
      fi
      sleep 5
      continue
    fi

    _rf_bin=""
    [ -x /data/harmony/bin/hub_rf ] && _rf_bin=/data/harmony/bin/hub_rf
    if [ -n "$_rf_bin" ]; then
      if ! pidof hub_rf >/dev/null 2>&1; then
        echo "$(date) Starting hub_rf daemon..." >> "$LOG"
        "$_rf_bin" >> "$LOG" 2>&1
        echo "$(date) hub_rf exited, retry in ${_delay}s" >> "$LOG"
        sleep "$_delay"
        [ "$_delay" -lt 60 ] && _delay=$(expr "$_delay" + "$_delay")
        continue
      fi
    fi
    _delay=5
    sleep 5
  done
) &

echo "$(date) harmony init done" >> "$LOG"