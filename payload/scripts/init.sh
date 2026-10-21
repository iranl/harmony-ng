#!/bin/sh
PATH=/data/codex/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
trap '' HUP
LOG=/tmp/codex-init.log

sync_fs() {
  sync 2>/dev/null || /data/codex/bin/codex_sync 2>/dev/null || /data/codex/bin/sync 2>/dev/null || true
}

echo "$(date) codex init start: $1" >> "$LOG"

case "$1" in
  stop)
    echo "$(date) Stopping codex services..." >> "$LOG"
    killall -9 codex_daemon codex_rf codex_btstack 2>/dev/null || true
    rm -f /tmp/codex_watchdog.pid 2>/dev/null || true
    exit 0
    ;;
  restart)
    echo "$(date) Restarting codex services..." >> "$LOG"
    killall -9 codex_daemon codex_rf codex_btstack 2>/dev/null || true
    sleep 2
    # If supervisor loops already running from boot, let them respawn daemons
    if ps | grep -v "$$" | grep "/data/codex/init.sh" >/dev/null 2>&1; then
      exit 0
    fi
    ;;
esac

# Avoid duplicate supervisor loop spawns
if ps | grep -v "$$" | grep "/data/codex/init.sh" >/dev/null 2>&1; then
  echo "$(date) codex supervisor already running" >> "$LOG"
  exit 0
fi
echo "$$" > /tmp/codex_watchdog.pid

# 0. Neuter watchdog and crash reporting immediately
touch /etc/nowatchdog 2>/dev/null || true
chmod -x /usr/bin/watchdog 2>/dev/null || true
killall -9 watchdog 2>/dev/null || true

export LD_PRELOAD=""

# Set kernel printk loglevel according to debug logging config
if [ -f /data/codex/debug_logging.conf ]; then
  echo "7 4 1 7" > /proc/sys/kernel/printk 2>/dev/null || true
else
  echo "3 4 1 7" > /proc/sys/kernel/printk 2>/dev/null || true
fi

# 1. Clean up legacy flash artifacts to minimize flash wear and reclaim space
rm -rf /data/codex-backups /data/codex/resource-backups /data/codex/bin/*.tmp-handoff* /data/*.tmp-handoff* 2>/dev/null || true
rm -rf /cache/*.log /cache/codex-init.log /cache/crashlog-*.json /tmp/codex_bthid_remote.pid 2>/dev/null || true
rm -f /data/*.ota* /data/*.tar.gz /data/*.zip /data/codex_elite.ota.gz /data/codex/*.log /data/codex/bin/*.old /data/codex/bin/*.bak 2>/dev/null || true
rm -f /data/codex/bin/pair_b25.sh /data/codex/bin/do_pair.sh /data/codex/bin/test_ble_diag /data/codex/bin/test_smp /data/codex/bin/test_hci_sniff /data/codex/bin/codex_ir_send /data/codex/bin/codex_webui /data/codex/bin/codex_rf_query /data/codex/bin/codex_rf_sniff /data/rf_pair_test /data/test_hub_tx /data/codex_rf 2>/dev/null || true
rm -f /data/codex/g_serial.ko /data/codex/mknod_bin /data/codex/mknod 2>/dev/null || true
rm -f /data/codex/modules/*.bak 2>/dev/null || true
for _mod in /data/codex/modules/*.ko; do
  [ -f "$_mod" ] || continue
  _b="${_mod##*/}"
  [ -f "/data/codex/modules/usb_net/$_b" ] && rm -f "$_mod"
done

# Migrate WebUI from /data/codex/www to /cache/www to preserve critical /data flash space
if [ -d /data/codex/www ] && [ ! -L /data/codex/www ]; then
  mkdir -p /cache/www 2>/dev/null
  cp -rf /data/codex/www/* /cache/www/ 2>/dev/null || true
  rm -rf /data/codex/www 2>/dev/null || true
fi
mkdir -p /cache/www/assets/icons 2>/dev/null || true
[ -L /data/codex/www ] || ln -sfn /cache/www /data/codex/www 2>/dev/null || true

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
    killall -9 luaworks luadraws lua netmonitor codex_bthid_remote 2>/dev/null || true
    sleep 1
  done
) &

# 3. Dropbear SSH Server & Shell Profile & Extended BusyBox
if [ -x /cache/bin/busybox ]; then
  /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true
  ln -sf /cache/bin/busybox /data/codex/bin/busybox 2>/dev/null || true
  for _tool in grep sed awk tar head tail which find xargs tr cut sort uniq du df; do
    ln -sf /cache/bin/busybox "/usr/bin/$_tool" 2>/dev/null || true
    ln -sf /cache/bin/busybox "/bin/$_tool" 2>/dev/null || true
  done
fi

mkdir -p /etc/profile.d 2>/dev/null || true
cat <<'EOF' > /etc/profile.d/00-codex.sh
export PATH=/cache/bin:/data/codex/bin:$PATH
EOF
chmod 644 /etc/profile.d/00-codex.sh 2>/dev/null || true

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
    export PATH=/cache/bin:/data/codex/bin:$PATH
    exec "$BB_PATH" sh -l
fi
export PATH=/cache/bin:/data/codex/bin:$PATH
EOF
  chmod 644 "$prof" 2>/dev/null || true
done

if [ -x /data/codex/bin/dropbear ]; then
  cat <<'EOF' > /usr/sbin/dropbear
#!/bin/sh
export PATH=/cache/bin:/data/codex/bin:$PATH
exec /data/codex/bin/dropbear -K 300 "$@"
EOF
  chmod 755 /usr/sbin/dropbear
  if ! ps | grep '[d]ropbear' >/dev/null 2>&1; then
    /usr/sbin/dropbear
  fi
fi

# 4. Disable and kill Logitech HAL (direct /dev/i2s IR active, saves ~2.5MB RAM)
chmod -x /usr/bin/hal 2>/dev/null || true
killall -9 hal 2>/dev/null || true

# 5. Network Time Sync (SNTP)
if [ -x /data/codex/bin/codex_sntp ]; then
  (
    for i in 1 2 3 4 5 6 7 8 9 10; do
      if /data/codex/bin/codex_sntp pool.ntp.org --set >> "$LOG" 2>&1; then
        echo "$(date) NTP time sync succeeded" >> "$LOG"
        break
      fi
      sleep 3
    done
  ) &
fi

# 5.2 Network Manager (USB Gadget 15s check, Ethernet startup & Wi-Fi fallback)
if [ -x /data/codex/network_manager.sh ]; then
  /data/codex/network_manager.sh boot >> "$LOG" 2>&1 &
elif [ -x /data/codex/bin/network_manager.sh ]; then
  /data/codex/bin/network_manager.sh boot >> "$LOG" 2>&1 &
fi


# 6. Unified Codex Daemon (WebUI on 8080, Orchestrator/WS/MQTT on 8089)
(
  _delay=5
  while true; do
    if [ -x /data/codex/bin/codex_daemon ]; then
      if ! pidof codex_daemon >/dev/null 2>&1; then
        echo "$(date) Starting codex_daemon..." >> "$LOG"
        /data/codex/bin/codex_daemon 8089 >> "$LOG" 2>&1
        echo "$(date) codex_daemon exited, retry in ${_delay}s" >> "$LOG"
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
killall -9 bluetoothd codex_bthid_remote codex_bthid_keyboard 2>/dev/null || true
rm -f /data/codex/bin/codex_bthid_remote /data/codex/bin/codex_bthid_keyboard 2>/dev/null || true
(
  _delay=5
  while true; do
    if [ -f /data/codex/bt_disabled.conf ]; then
      if pidof codex_btstack >/dev/null 2>&1; then
        echo "$(date) BT disabled flag detected, stopping BTstack..." >> "$LOG"
        killall -9 codex_btstack 2>/dev/null || true
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

    if [ -x /data/codex/bin/codex_btstack ]; then
      if ! pidof codex_btstack >/dev/null 2>&1; then
        echo "$(date) Starting BTstack backend..." >> "$LOG"
        /data/codex/bin/codex_btstack >> "$LOG" 2>&1
        echo "$(date) codex_btstack exited, retry in ${_delay}s" >> "$LOG"
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
    if [ -f /data/codex/rf_disabled.conf ]; then
      if pidof codex_rf >/dev/null 2>&1; then
        echo "$(date) RF disabled flag detected, stopping codex_rf..." >> "$LOG"
        killall -9 codex_rf 2>/dev/null || true
      fi
      sleep 5
      continue
    fi

    if [ -x /data/codex/bin/codex_rf ]; then
      if ! pidof codex_rf >/dev/null 2>&1; then
        echo "$(date) Starting codex_rf daemon..." >> "$LOG"
        /data/codex/bin/codex_rf >> "$LOG" 2>&1
        echo "$(date) codex_rf exited, retry in ${_delay}s" >> "$LOG"
        sleep "$_delay"
        [ "$_delay" -lt 60 ] && _delay=$(expr "$_delay" + "$_delay")
        continue
      fi
    fi
    _delay=5
    sleep 5
  done
) &

echo "$(date) codex init done" >> "$LOG"