#!/bin/sh
PATH=/data/codex/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
LOG=/tmp/codex-recovery.log
SSID_PREFIX=Harmony-Recovery
AP_IF=ath1
AP_IP=192.168.76.1

log() {
  echo "$(date) $*" >> "$LOG"
}

ssid() {
  mac=$(cat /sys/class/net/wifi0/address 2>/dev/null | tr -d ':' | cut -c7-12)
  [ -n "$mac" ] || mac=hub
  echo "${SSID_PREFIX}-${mac}"
}

CONF=/data/codex/recovery_ap.conf
WPA_PASSPHRASE=harmonyrecovery
WPA_ENABLED=1

if [ -f "$CONF" ]; then
  . "$CONF"
fi

stop_station() {
  killall -9 netmonitor 2>/dev/null || true
  killall -9 wpa_supplicant 2>/dev/null || true
  killall -9 udhcpc 2>/dev/null || true
  ifconfig ath0 down 2>/dev/null
  wlanconfig ath0 destroy 2>/dev/null
}

start_ap() {
  if [ -f /var/run/codex-recovery-ap.pid ]; then
    old=$(cat /var/run/codex-recovery-ap.pid 2>/dev/null)
    if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
      log "recovery ap already running"
      return 0
    fi
  fi

  cur_ssid=$(ssid)
  log "starting recovery ap ($cur_ssid)"
  stop_station
  wlanconfig "$AP_IF" destroy 2>/dev/null
  wlanconfig "$AP_IF" create wlandev wifi0 wlanmode ap >> "$LOG" 2>&1
  iwpriv "$AP_IF" mode 11ng >> "$LOG" 2>&1
  iwconfig "$AP_IF" essid "$cur_ssid" channel 6 >> "$LOG" 2>&1
  ifconfig "$AP_IF" "$AP_IP" netmask 255.255.255.0 up >> "$LOG" 2>&1

  # WPA2 security configuration
  if [ "$WPA_ENABLED" = "1" ] && [ -n "$WPA_PASSPHRASE" ] && [ ${#WPA_PASSPHRASE} -ge 8 ]; then
    HCONF=/tmp/hostapd-recovery.conf
    cat <<EOF > "$HCONF"
interface=$AP_IF
ctrl_interface=/var/run/hostapd
ssid=$cur_ssid
channel=6
wpa=2
wpa_passphrase=$WPA_PASSPHRASE
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP
EOF
    HOSTAPD_BIN=""
    for b in /data/codex/bin/hostapd /usr/sbin/hostapd /usr/bin/hostapd; do
      if [ -x "$b" ]; then HOSTAPD_BIN="$b"; break; fi
    done
    if [ -n "$HOSTAPD_BIN" ]; then
      killall -9 hostapd 2>/dev/null || true
      "$HOSTAPD_BIN" -B "$HCONF" >> "$LOG" 2>&1 || "$HOSTAPD_BIN" "$HCONF" >> "$LOG" 2>&1 &
      log "recovery ap WPA2 enabled via $HOSTAPD_BIN"
    else
      # Try madwifi iwpriv WPA2 mode
      iwpriv "$AP_IF" authmode 3 >> "$LOG" 2>&1 || true
      log "recovery ap hostapd not found, applied driver authmode"
    fi
  else
    log "recovery ap running in open mode"
  fi

  killall -9 codex_dhcpd codex_portal 2>/dev/null || true
  /data/codex/bin/codex_dhcpd "$AP_IF" "$AP_IP" 192.168.76.100 192.168.76.150 >> "$LOG" 2>&1 &
  /data/codex/bin/codex_portal 80 >> "$LOG" 2>&1 &
  echo $! > /var/run/codex-recovery-ap.pid
  echo 3 > /sys/class/input/input0/led 2>/dev/null
}

stop_ap() {
  log "stopping recovery ap"
  killall -9 codex_portal codex_dhcpd hostapd 2>/dev/null || true
  rm -f /var/run/codex-recovery-ap.pid /tmp/hostapd-recovery.conf
  ifconfig "$AP_IF" down 2>/dev/null
  wlanconfig "$AP_IF" destroy 2>/dev/null
}

monitor() {
  count=0
  while true; do
    val=$(cat /sys/class/input/input0/tde 2>/dev/null)
    case "$val" in
      1*) count=$(expr "$count" + 1) ;;
      *) count=0 ;;
    esac
    if [ "$count" -ge 5 ]; then
      start_ap
      count=0
      sleep 20
    fi
    sleep 1
  done
}

case "$1" in
  monitor) monitor ;;
  stop) stop_ap ;;
  start|boot-tde|"") start_ap ;;
  *) echo "usage: $0 [start|stop|boot-tde|monitor]" ;;
esac
