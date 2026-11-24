#!/bin/sh
PATH=/data/harmony/bin:/cache/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
trap '' HUP
# run from a plugin (or a shell that exported these): hub_daemon and the other services must
# not look like plugin processes to the stop sweeps. unset alone is not enough: a forked
# ( ... ) & supervisor keeps this shell's exec-time /proc/<pid>/environ, so run again clean.
if [ -n "$HARMONY_PLUGIN_ID$HARMONY_SOCKET$HARMONY_PLUGIN_DATA" ]; then
  unset HARMONY_PLUGIN_ID HARMONY_SOCKET HARMONY_PLUGIN_DATA
  exec /bin/sh "$0" "$@"
fi
LOG=/tmp/harmony-init.log

sync_fs() {
  /data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true
}

# / is a union over /data, so these are flash writes: a file is rewritten only when its content
# changes (every start used to rewrite them), through a temporary name, so a power cut never
# leaves it empty. Usage: write_if_changed PATH MODE
write_if_changed() {
  _wic_new=$(cat)
  if [ -f "$1" ] && [ "$(cat "$1" 2>/dev/null)" = "$_wic_new" ]; then
    return 0
  fi
  printf '%s\n' "$_wic_new" > "$1.tmp" && chmod "$2" "$1.tmp" && mv -f "$1.tmp" "$1"
}

# A symlink is replaced only when it does not point there already. Usage: link_if_changed TARGET LINK
# The stock BusyBox has no readlink (the extended one in /cache/bin has): ls -l tells it then.
link_target() {
  readlink "$1" 2>/dev/null || ls -l "$1" 2>/dev/null | sed -n 's/.* -> //p'
}
link_if_changed() {
  if [ -L "$2" ] && [ "$(link_target "$2")" = "$1" ]; then
    return 0
  fi
  ln -sf "$1" "$2"
}

echo "$(date) harmony init start: $1" >> "$LOG"

case "$1" in
  stop)
    echo "$(date) Stopping harmony services..." >> "$LOG"
    touch /tmp/harmony_stop
    # Gracefully terminate hub_rf so it releases SPI device and closes /dev/rfspi
    killall -15 hub_rf 2>/dev/null || true
    sleep 1
    # Terminate recorded supervisor loops
    if [ -f /tmp/harmony_supervisors.pid ]; then
      for _spid in $(cat /tmp/harmony_supervisors.pid 2>/dev/null); do
        kill -9 "$_spid" 2>/dev/null || true
      done
      rm -f /tmp/harmony_supervisors.pid 2>/dev/null || true
    fi
    # Terminate any legacy or untracked init.sh subshell loops, but not the caller: a shell
    # that runs "init.sh stop" (such as deploy_hub over SSH) has init.sh in its command line
    for _p in $(ps | grep 'init\.sh' | grep -v 'grep' | sed 's/^[ ]*//' | cut -d' ' -f1 2>/dev/null); do
      [ -n "$_p" ] && [ "$_p" != "$$" ] && [ "$_p" != "$PPID" ] && kill -9 "$_p" 2>/dev/null || true
    done
    # Plugins are children of hub_daemon: on SIGTERM it stops each one (SIGTERM, 1 s, SIGKILL)
    killall -15 hub_daemon 2>/dev/null || true
    sleep 2
    # Terminate all daemon processes
    killall -9 hub_rf hub_daemon hub_btstack hub_usbwifi hal luaworks luadraws lua 2>/dev/null || true
    # Plugin processes still left (hub_daemon was hung or already gone, or a helper still runs
    # the EXIT trap of hub_daemon's SIGTERM): found by the HARMONY_SOCKET that hub_daemon puts
    # in their environment. A shell or a program started by hand in a plugin folder has none
    # (a sweep by working directory killed the caller's shell). This script and the shells
    # that started it are never taken. Up to 5 s to finish on their own (the ps5-wake helper
    # restores the Hub's Bluetooth address and removes its lock), then SIGTERM, 1 s, SIGKILL
    # to the ones that still have the plugin environment (a pid freed and reused is not).
    _keep=" $$ "
    _a=$PPID
    _i=0
    while [ -n "$_a" ] && [ "$_a" -gt 1 ] 2>/dev/null && [ $_i -lt 32 ]; do
      _keep="$_keep$_a "
      _a=$(awk '/^PPid:/ { print $2 }' "/proc/$_a/status" 2>/dev/null)
      _i=$((_i + 1))
    done
    plugin_pids() {
      for _e in /proc/[0-9]*/environ; do
        _p=${_e#/proc/}
        _p=${_p%/environ}
        case "$_keep" in *" $_p "*) continue ;; esac
        tr '\000' '\n' < "$_e" 2>/dev/null | grep -q '^HARMONY_SOCKET=/tmp/plugins/' && echo "$_p"
      done
    }
    _left=$(plugin_pids)
    _i=0
    while [ -n "$_left" ] && [ $_i -lt 5 ]; do
      sleep 1
      _i=$((_i + 1))
      _left=$(plugin_pids)
    done
    if [ -n "$_left" ]; then
      kill -15 $_left 2>/dev/null || true
      sleep 1
      _left=$(plugin_pids)
      [ -n "$_left" ] && kill -9 $_left 2>/dev/null || true
    fi
    rm -f /tmp/plugins/*.sock 2>/dev/null || true
    # the USB Wi-Fi setup started as "hub_util usbwifi" (no hub_usbwifi link) runs as hub_util
    for _p in $(pidof hub_util 2>/dev/null); do
      case "$(tr '\000' ' ' < "/proc/$_p/cmdline" 2>/dev/null)" in
        *" usbwifi"*) kill -9 "$_p" 2>/dev/null || true ;;
      esac
    done
    rm -f /tmp/harmony_watchdog.pid 2>/dev/null || true
    sync_fs
    exit 0
    ;;
  restart)
    echo "$(date) Restarting harmony services..." >> "$LOG"
    $0 stop
    sleep 1
    $0 start &
    exit 0
    ;;
  start)
    # Continue below
    ;;
esac

# Clear stop trigger flag
rm -f /tmp/harmony_stop 2>/dev/null || true

# Avoid duplicate supervisor loop spawns
if [ -f /tmp/harmony_supervisors.pid ]; then
  _any_alive=0
  for _spid in $(cat /tmp/harmony_supervisors.pid 2>/dev/null); do
    if [ -d "/proc/$_spid" ]; then
      _any_alive=1
      break
    fi
  done
  if [ "$_any_alive" = "1" ]; then
    echo "$(date) harmony supervisors already running" >> "$LOG"
    exit 0
  fi
fi
rm -f /tmp/harmony_supervisors.pid 2>/dev/null || true
echo "$$" > /tmp/harmony_watchdog.pid

# 5-minute stability timer in background
(
  sleep 300
  echo 0 > /data/harmony/reboot_counter 2>/dev/null || true
  sync_fs
  echo "$(date) Hub uptime > 5m achieved, reboot counter reset" >> /tmp/boot_stability.log
) &

# 0. Neuter watchdog and crash reporting immediately
[ -e /etc/nowatchdog ] || touch /etc/nowatchdog 2>/dev/null || true
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
mkdir -p /data/harmony/bin /data/harmony/config /data/harmony/plugins /tmp/plugins 2>/dev/null || true
if [ -x /cache/bin/busybox ]; then
  /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true
  link_if_changed /cache/bin/busybox /data/harmony/bin/busybox 2>/dev/null || true
fi
link_if_changed /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true
link_if_changed /data/harmony/bin/hub_sync /cache/bin/sync 2>/dev/null || true

[ -f /data/harmony/bin/safe_reboot.sh ] && chmod +x /data/harmony/bin/safe_reboot.sh 2>/dev/null || true
[ -f /data/harmony/safe_reboot.sh ] && link_if_changed /data/harmony/safe_reboot.sh /data/harmony/bin/safe_reboot.sh 2>/dev/null || true
# the web UI's factory reset runs /data/harmony/bin/factory_reset.sh; the installers put it here
[ -f /data/harmony/factory_reset.sh ] && link_if_changed /data/harmony/factory_reset.sh /data/harmony/bin/factory_reset.sh 2>/dev/null || true

mkdir -p /etc/profile.d 2>/dev/null || true
write_if_changed /etc/profile.d/00-harmony.sh 644 <<'EOF'
export PATH=/data/harmony/bin:/cache/bin:$PATH
EOF

for prof in /root/.profile /home/root/.profile; do
  mkdir -p "$(dirname "$prof")" 2>/dev/null || true
  write_if_changed "$prof" 644 <<'EOF'
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
done

# SSH: /usr/sbin/dropbear is a copy of /data/harmony/hub_sshd (key logins only), as rcS.local sets
# it at boot, written only when it differs. A server that already runs keeps its options until it
# is started again (the next boot).
_sshd_bin=""
if [ -s /data/harmony/hub_sshd ] && grep -q '^# harmony-ng hub_sshd$' /data/harmony/hub_sshd 2>/dev/null; then
  _sshd_bin=$(sh /data/harmony/hub_sshd --binary 2>/dev/null)
fi
if [ -n "$_sshd_bin" ] && [ -x "$_sshd_bin" ]; then
  _sshd_cur=""
  [ -f /usr/sbin/dropbear ] && [ ! -L /usr/sbin/dropbear ] && _sshd_cur=$(cat /usr/sbin/dropbear 2>/dev/null)
  if [ "$_sshd_cur" != "$(cat /data/harmony/hub_sshd)" ]; then
    cp /data/harmony/hub_sshd /usr/sbin/dropbear.new 2>/dev/null && chmod 755 /usr/sbin/dropbear.new &&
      mv -f /usr/sbin/dropbear.new /usr/sbin/dropbear
  fi
  if ! ps | grep '[d]ropbear' >/dev/null 2>&1; then
    # not with this script's ignored SIGHUP: SSH sessions would inherit it. The wrapper directly
    # when /usr/sbin/dropbear could not be written
    if [ -x /usr/sbin/dropbear ] && [ -s /usr/sbin/dropbear ]; then
      (trap - HUP; /usr/sbin/dropbear)
    else
      (trap - HUP; sh /data/harmony/hub_sshd)
    fi
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

# 5.1 Wi-Fi setup over USB. The stock /usr/bin/usbhid (started by the stock boot script) shows
# the Hub to a computer as its USB HID device and relays the reports to a program on
# 127.0.0.1:16716, which was the stock hal (not run by harmony-ng). hub_usbwifi (hub_util) listens
# there and answers the Wi-Fi requests only (set and read /sys/wifi/connect). Started before the
# network manager: it keeps the USB port a USB device while a computer is using it at boot.
link_if_changed /data/harmony/bin/hub_util /data/harmony/bin/hub_usbwifi 2>/dev/null || true
# USB_WIFI_SETUP=0 in ethernet.conf (POST /api/ethernet-save usb_wifi_setup) turns it off
if tr -d ' \t"\r' < /data/harmony/ethernet.conf 2>/dev/null | grep -q '^USB_WIFI_SETUP=0$'; then
  echo "$(date) USB Wi-Fi setup is turned off in the settings; hub_usbwifi not started" >> "$LOG"
elif [ -x /data/harmony/bin/hub_util ]; then
(
  _delay=5
  while true; do
    [ -f /tmp/harmony_stop ] && exit 0
    # Not the first start of this boot (init.sh restart, an exit): usbhid still holds its
    # connection to the old listener, so it is started again while the port is a USB device.
    # The first time, the stock boot script starts it. Never once the boot switched the port to
    # host mode (start_usb_eth.sh writes "host" before it removes the device driver, which can
    # stay loaded when that fails).
    if [ -f /tmp/harmony_usbwifi.started ] && [ -x /usr/bin/usbhid ] &&
       [ "$(cat /tmp/harmony_usb_port_mode 2>/dev/null)" != "host" ] &&
       grep -q '^ath_udc ' /proc/modules 2>/dev/null; then
      killall -9 usbhid 2>/dev/null || true
      # a killed usbhid may hold the gadget for a moment: gadgetfs cannot be unmounted and the
      # new usbhid cannot claim it while it does (as in start_usb_eth.sh)
      _i=0
      while [ "$_i" -lt 5 ] && pidof usbhid >/dev/null 2>&1; do
        sleep 1
        _i=$(expr "$_i" + 1)
      done
      umount /dev/gadget 2>/dev/null || true
      (trap - HUP; /usr/bin/usbhid -s >/dev/null 2>&1 &)
    fi
    touch /tmp/harmony_usbwifi.started
    echo "$(date) Starting hub_usbwifi..." >> "$LOG"
    _started=$(date +%s)
    if [ -x /data/harmony/bin/hub_usbwifi ]; then
      /data/harmony/bin/hub_usbwifi >> "$LOG" 2>&1
    else
      /data/harmony/bin/hub_util usbwifi >> "$LOG" 2>&1
    fi
    [ -f /tmp/harmony_stop ] && exit 0
    _ran=$(expr "$(date +%s)" - "$_started" 2>/dev/null || echo 0)
    [ "$_ran" -ge 60 ] 2>/dev/null && _delay=5
    echo "$(date) hub_usbwifi exited, retry in ${_delay}s" >> "$LOG"
    sleep "$_delay"
    [ "$_delay" -lt 60 ] && _delay=$(expr "$_delay" + "$_delay")
  done
) &
echo "$!" >> /tmp/harmony_supervisors.pid
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
    [ -f /tmp/harmony_stop ] && exit 0
    _daemon=""
    [ -x /data/harmony/bin/hub_daemon ] && _daemon=/data/harmony/bin/hub_daemon
    if [ -n "$_daemon" ]; then
      if ! pidof hub_daemon >/dev/null 2>&1; then
        echo "$(date) Starting hub_daemon..." >> "$LOG"
        _started=$(date +%s)
        "$_daemon" 8089 >> "$LOG" 2>&1
        [ -f /tmp/harmony_stop ] && exit 0
        # A daemon that ran for a minute or more was restarted (a deploy, a kill), not
        # crash-looping: come back after the base delay. The delay used to double on
        # every exit and reset only while the daemon was found running, so after a
        # few restarts every restart took 80 s.
        _ran=$(expr "$(date +%s)" - "$_started" 2>/dev/null || echo 0)
        [ "$_ran" -ge 60 ] 2>/dev/null && _delay=5
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
echo "$!" >> /tmp/harmony_supervisors.pid

# 7. BTstack Bluetooth Engine (BlueZ 4 permanently disabled)
chmod -x /usr/sbin/bluetoothd 2>/dev/null || true
killall -9 bluetoothd 2>/dev/null || true
(
  _delay=5
  while true; do
    [ -f /tmp/harmony_stop ] && exit 0
    if [ -f /data/harmony/bt_disabled.conf ]; then
      if pidof hub_btstack >/dev/null 2>&1; then
        echo "$(date) BT disabled flag detected, stopping BTstack..." >> "$LOG"
        killall -9 hub_btstack 2>/dev/null || true
        hciconfig hci0 down 2>/dev/null || true
      fi
      sleep 5
      continue
    fi

    # If locked by an exclusive radio task (e.g. ps5-wake spoof sequence), pause supervisor.
    # A helper killed with -9 never removes its lock (no EXIT trap): after 120 s the lock
    # is taken as stale and removed, or Bluetooth stayed off until a reboot.
    if [ -f /tmp/hub_btstack.lock ]; then
      _lock_wait=$(( ${_lock_wait:-0} + 2 ))
      if [ "$_lock_wait" -ge 120 ]; then
        echo "$(date) /tmp/hub_btstack.lock older than 120 s, removing it" >> "$LOG"
        rm -f /tmp/hub_btstack.lock
        _lock_wait=0
      else
        sleep 2
        continue
      fi
    else
      _lock_wait=0
    fi

    # Re-attach Bluetooth radio if hciattach died or hci0 device is missing.
    # rcS.local sets the radio up in the background at boot, next to this loop: in the
    # first minute of uptime, or while a bccmd runs, the radio is left to it (hub_btstack
    # waits up to 15 s for hci0 by itself).
    _up=$(cut -d. -f1 /proc/uptime 2>/dev/null)
    if [ "${_up:-0}" -ge 60 ] 2>/dev/null && ! pidof bccmd >/dev/null 2>&1 && \
       { ! pidof hciattach >/dev/null 2>&1 || ! hciconfig hci0 >/dev/null 2>&1; }; then
      echo "$(date) Bluetooth radio not attached (hciattach/hci0 missing), re-attaching..." >> "$LOG"
      killall -9 hub_btstack 2>/dev/null || true
      killall -9 hciattach 2>/dev/null || true
      sleep 1

      # A file present means the boot set-up ran: the chip then runs its keys at 115200.
      # A chip in its power-on state talks at 38400 (bccmd's default).
      _bcsp="-t bcsp -b 115200"
      if [ ! -f /tmp/pskeys.psr ]; then
        _mfg=/dev/$(grep mfg /proc/mtd 2>/dev/null | cut -d: -f1)
        _hw_ver=$(sed -n '/hw_ver/{N N N N s/^[^0-9]*\([0-9][0-9]\)/\1/p}' "$_mfg" 2>/dev/null)
        _bcsp="-t bcsp"
        _bname=$(bccmd $_bcsp buildname 2>/dev/null) || {
          _bcsp="-t bcsp -b 115200"
          _bname=$(bccmd $_bcsp buildname 2>/dev/null)
        }
        case "$_bname" in
          *A06*) _pskeys=csr8811-a06.psr ;;
          *A08*) case "$_hw_ver" in
            00.40) _pskeys=csr8811-a08-pb1.psr ;;
            *)     _pskeys=csr8811-a08.psr ;;
          esac ;;
          *) _pskeys="" ;;
        esac
        if [ -n "$_pskeys" ] && [ -f "/lib/firmware/$_pskeys" ]; then
          cat "/lib/firmware/$_pskeys" > /tmp/pskeys.psr.new
          for _i in 1 2 3 4 5; do
            [ -f /sys/class/net/ath0/address ] && break
            sleep 1
          done
          cat /sys/class/net/ath0/address 2>/dev/null | sed 's/\([0-9a-f][0-9a-f]\):\([0-9a-f][0-9a-f]\):\([0-9a-f][0-9a-f]\):\([0-9a-f][0-9a-f]\):\([0-9a-f][0-9a-f]\):\([0-9a-f][0-9a-f]\).*/\&0x0001 = 0x\4 0x\5\6 0x\3 0x\1\2/' >> /tmp/pskeys.psr.new
          mv /tmp/pskeys.psr.new /tmp/pskeys.psr
        fi
      fi
      if [ -f /tmp/pskeys.psr ] && [ ! -f /tmp/hub_btstack.lock ]; then
        bccmd $_bcsp psload -r /tmp/pskeys.psr 2>/dev/null || \
          { [ "$_bcsp" != "-t bcsp" ] && bccmd -t bcsp psload -r /tmp/pskeys.psr 2>/dev/null; } || true
        sleep 1
      fi

      # A radio helper may have taken the UART meanwhile, or the line got attached
      [ -f /tmp/hub_btstack.lock ] && continue
      hciconfig hci0 >/dev/null 2>&1 || hciattach ttyS0 bcsp 2>/dev/null || true
      sleep 1
      if ! hciconfig hci0 >/dev/null 2>&1; then
        echo "$(date) Bluetooth radio did not attach, retry in ${_delay}s" >> "$LOG"
        sleep "$_delay"
        [ "$_delay" -lt 60 ] && _delay=$(expr "$_delay" + "$_delay")
        continue
      fi
    fi

    # Ensure hci0 is UP
    if ! hciconfig hci0 2>/dev/null | grep -q 'UP'; then
      hciconfig hci0 up 2>/dev/null || true
      hciconfig hci0 sspmode 1 2>/dev/null || true
      sleep 1
    fi

    _bt_bin=""
    [ -x /data/harmony/bin/hub_btstack ] && _bt_bin=/data/harmony/bin/hub_btstack
    if [ -n "$_bt_bin" ]; then
      if ! pidof hub_btstack >/dev/null 2>&1; then
        echo "$(date) Starting BTstack backend..." >> "$LOG"
        _started=$(date +%s)
        "$_bt_bin" >> "$LOG" 2>&1
        [ -f /tmp/harmony_stop ] && exit 0
        # A daemon that ran for a minute or more was restarted (a deploy, a kill), not
        # crash-looping: come back after the base delay. The delay used to double on
        # every exit and reset only while the daemon was found running, so after a
        # few restarts every restart took 80 s.
        _ran=$(expr "$(date +%s)" - "$_started" 2>/dev/null || echo 0)
        [ "$_ran" -ge 60 ] 2>/dev/null && _delay=5
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
echo "$!" >> /tmp/harmony_supervisors.pid

# 8. CC2544 RF Daemon (Logitech HAL replacement for the remote RF link)
(
  _delay=5
  while true; do
    [ -f /tmp/harmony_stop ] && exit 0
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
        rm -f /tmp/icon_push.* 2>/dev/null # an icon file of a transfer that a kill cut short
        _started=$(date +%s)
        "$_rf_bin" >> "$LOG" 2>&1
        [ -f /tmp/harmony_stop ] && exit 0
        # A daemon that ran for a minute or more was restarted (a deploy, a kill), not
        # crash-looping: come back after the base delay. The delay used to double on
        # every exit and reset only while the daemon was found running, so after a
        # few restarts every restart took 80 s.
        _ran=$(expr "$(date +%s)" - "$_started" 2>/dev/null || echo 0)
        [ "$_ran" -ge 60 ] 2>/dev/null && _delay=5
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
echo "$!" >> /tmp/harmony_supervisors.pid

# 9. Log cap: the logs live in tmpfs, which is RAM on a 32 MB Hub. Unbounded, $LOG starved the
# kernel (12 MB of log, 1 MB free, OOM kills of hub_rf and hub_daemon mid key press), and the
# network monitor writes its log every few seconds while the network is down. Every writer
# appends (>>), so truncating in place is safe; the last 64 KB are kept in <log>.1. A log over
# 256 KB is cut, looked at every 20 s: with RF debug logging on, hub_rf writes about 2 MB a minute.
cap_log() {
  # stock applets only (ls, awk): /cache and its extended BusyBox may not be there
  _sz=$(ls -l "$1" 2>/dev/null | awk '{print $5}')
  [ -n "$_sz" ] || _sz=0
  if [ "$_sz" -gt 262144 ] 2>/dev/null; then
    # keep the last 64 KB in <log>.1, then truncate in place. Without dd (only the extended
    # BusyBox has it) no tail is kept: a copy of the whole log doubled the RAM it took. The
    # stock ash has no "command" builtin, so dd is simply tried.
    dd if="$1" of="$1.1" bs=1024 skip=$(expr "$_sz" / 1024 - 64) 2>/dev/null || rm -f "$1.1"
    : > "$1"
    echo "$(date) log capped: was ${_sz} bytes, last 64 KB in $1.1" >> "$1"
  fi
}
(
  while true; do
    sleep 20
    [ -f /tmp/harmony_stop ] && exit 0
    for _log in "$LOG" /tmp/harmony-network.log; do
      cap_log "$_log"
    done
  done
) &
echo "$!" >> /tmp/harmony_supervisors.pid

# hub_fetch or libbearssl.so.0 missing (an update from v2.5.3 or older does not install them):
# fetch the ones of this release once the network is up. Up to 6 tries, 10 minutes apart.
if { [ ! -x /data/harmony/bin/hub_fetch ] || [ ! -s /data/harmony/lib/libbearssl.so.0 ]; } && \
   [ -x /data/harmony/update.sh ]; then
  (
    # 6 tries 10 minutes apart, then one an hour (the network may come back much later). The log
    # (RAM) keeps the first 6 tries, then only the latest one.
    _try=0
    sleep 120
    while :; do
      [ -f /tmp/harmony_stop ] && exit 0
      [ $_try -ge 6 ] && : > /tmp/update_repair.log
      /bin/sh /data/harmony/update.sh repair-tls >> /tmp/update_repair.log 2>&1 && exit 0
      _try=$((_try + 1))
      if [ $_try -lt 6 ]; then sleep 600; else sleep 3600; fi
    done
  ) &
  echo "$!" >> /tmp/harmony_supervisors.pid
fi

echo "$(date) harmony init done" >> "$LOG"