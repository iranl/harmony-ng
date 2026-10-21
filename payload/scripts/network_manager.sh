#!/bin/sh
# Codex Network Manager - Ethernet / USB Host & Wi-Fi supervisor
# Handles 15s USB gadget safety boot, Ethernet initialization, and Wi-Fi fallback.

PATH=/data/codex/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
CONF=/data/codex/ethernet.conf
WPA_CONF=/etc/wpa_supplicant.ath0.conf
[ -f "$WPA_CONF" ] || WPA_CONF=/etc/wpa_supplicant.conf

LOG=/tmp/codex-network.log
STATE_FILE=/tmp/codex_active_net_state

log() {
  echo "$(date '+%Y-%m-%d %H:%M:%S') [NET] $*" >> "$LOG"
}

# Load configuration with safe defaults
ETH_ENABLED=1
ETH_FALLBACK_WIFI=1
ETH_MODE="dhcp"
ETH_IP=""
ETH_NETMASK=""
ETH_GATEWAY=""
ETH_DNS=""

load_config() {
  if [ -f "$CONF" ]; then
    # Parse KEY=VALUE safely
    while IFS='=' read -r key val || [ -n "$key" ]; do
      key=$(echo "$key" | tr -d ' \t\r\n')
      val=$(echo "$val" | tr -d '\r\n"')
      case "$key" in
        ETH_MODE) ETH_MODE="$val" ;;
        ETH_IP) ETH_IP="$val" ;;
        ETH_NETMASK) ETH_NETMASK="$val" ;;
        ETH_GATEWAY) ETH_GATEWAY="$val" ;;
        ETH_DNS) ETH_DNS="$val" ;;
      esac
    done < "$CONF"
  fi
}

configure_arp_isolation() {
  # Prevent ARP flux and broadcast loops when both eth and wifi are on same subnet
  echo 1 > /proc/sys/net/ipv4/conf/all/arp_ignore 2>/dev/null || true
  echo 1 > /proc/sys/net/ipv4/conf/default/arp_ignore 2>/dev/null || true
  echo 2 > /proc/sys/net/ipv4/conf/all/arp_announce 2>/dev/null || true
  echo 2 > /proc/sys/net/ipv4/conf/default/arp_announce 2>/dev/null || true
  echo 2 > /proc/sys/net/ipv4/conf/all/rp_filter 2>/dev/null || true
  echo 2 > /proc/sys/net/ipv4/conf/default/rp_filter 2>/dev/null || true
}

find_eth_interface() {
  # Ethernet on Harmony Hub is strictly USB-based.
  # Ignore internal SoC eth0 and devboard eth1 (no physical ports).
  for ifpath in /sys/class/net/*; do
    [ -d "$ifpath" ] || continue
    ifname="${ifpath##*/}"
    case "$ifname" in
      eth*|usb*)
        if [ "$ifname" != "eth0" ] && [ "$ifname" != "eth1" ] && [ "$ifname" != "wifi0" ] && [ "$ifname" != "ath0" ] && [ "$ifname" != "lo" ]; then
          echo "$ifname"
          return 0
        fi
        ;;
    esac
  done
  return 1
}

ensure_wifi() {
  log "Ensuring Wi-Fi connection (ath0)..."
  if ! ifconfig ath0 >/dev/null 2>&1; then
    /sbin/wlanconfig ath0 create wlandev wifi0 wlanmode sta 2>/dev/null || true
  fi
  if ! ps | grep '[w]pa_supplicant' >/dev/null 2>&1; then
    /usr/sbin/wpa_supplicant -s -iath0 -c "$WPA_CONF" >/dev/null 2>&1 &
    sleep 2
  fi
  ifconfig ath0 up 2>/dev/null || true
  
  # Check if ath0 already has IP
  wifi_ip=$(ifconfig ath0 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
  if [ -z "$wifi_ip" ]; then
    log "Requesting DHCP lease on ath0..."
    if ! pidof udhcpc >/dev/null 2>&1; then
      udhcpc -i ath0 -p /var/run/udhcpc.ath0.pid -b -t 5 -T 3 -A 15 >/dev/null 2>&1 &
    fi
    for i in 1 2 3 4 5 6 7 8; do
      wifi_ip=$(ifconfig ath0 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
      [ -n "$wifi_ip" ] && break
      sleep 1
    done
  fi

  if [ -n "$wifi_ip" ]; then
    log "Wi-Fi connected successfully: ath0 has IP $wifi_ip"
    echo "ath0:wifi:$wifi_ip" > "$STATE_FILE"
    return 0
  else
    log "Wi-Fi IP lease pending in background"
    return 1
  fi
}

start_eth() {
  log "Switching USB to host mode and loading drivers..."
  if [ -x /data/codex/bin/start_usb_eth.sh ]; then
    /data/codex/bin/start_usb_eth.sh >> "$LOG" 2>&1
  elif [ -x /data/codex/start_usb_eth.sh ]; then
    /data/codex/start_usb_eth.sh >> "$LOG" 2>&1
  elif [ -x /usr/sbin/start_usb_eth.sh ]; then
    /usr/sbin/start_usb_eth.sh >> "$LOG" 2>&1
  else
    log "Error: start_usb_eth.sh script not found!"
    return 1
  fi

  # Wait up to 8s for Ethernet interface to appear
  eth_if=""
  for i in 1 2 3 4 5 6 7 8; do
    eth_if=$(find_eth_interface)
    [ -n "$eth_if" ] && break
    sleep 1
  done

  if [ -z "$eth_if" ]; then
    log "No USB Ethernet adapter found on USB bus"
    return 1
  fi

  log "Found USB Ethernet interface: $eth_if"
  configure_arp_isolation
  ifconfig "$eth_if" up >> "$LOG" 2>&1

  # Check link/carrier (up to 5s)
  carrier_ok=0
  for i in 1 2 3 4 5; do
    cfile="/sys/class/net/$eth_if/carrier"
    if [ -f "$cfile" ] && [ "$(cat "$cfile" 2>/dev/null)" = "1" ]; then
      carrier_ok=1
      break
    fi
    sleep 1
  done

  if [ "$carrier_ok" != "1" ]; then
    log "Ethernet link down / cable unplugged on $eth_if"
    return 2
  fi

  # Configure IP
  if [ "$ETH_MODE" = "static" ] && [ -n "$ETH_IP" ]; then
    log "Configuring static IP $ETH_IP on $eth_if..."
    mask_arg=""
    [ -n "$ETH_NETMASK" ] && mask_arg="netmask $ETH_NETMASK"
    ifconfig "$eth_if" "$ETH_IP" $mask_arg up >> "$LOG" 2>&1
    if [ -n "$ETH_GATEWAY" ]; then
      route del default 2>/dev/null || true
      route add default gw "$ETH_GATEWAY" dev "$eth_if" >> "$LOG" 2>&1
    fi
    if [ -n "$ETH_DNS" ]; then
      echo "nameserver $ETH_DNS" > /etc/resolv.conf
    fi
  else
    log "Requesting DHCP lease on $eth_if..."
    udhcpc -i "$eth_if" -n -q -t 5 -T 3 >> "$LOG" 2>&1
  fi

  eth_ip=$(ifconfig "$eth_if" 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
  if [ -n "$eth_ip" ]; then
    configure_arp_isolation
    log "Ethernet connected successfully: $eth_if has IP $eth_ip"
    echo "$eth_if:ethernet:$eth_ip" > "$STATE_FILE"
    # Ensure default route prioritizes Ethernet over ath0
    route del default dev ath0 2>/dev/null || true
    return 0
  else
    log "DHCP failed to obtain IP on $eth_if"
    return 3
  fi
}

stop_eth() {
  log "Stopping Ethernet..."
  if [ -x /data/codex/bin/stop_usb_eth.sh ]; then
    /data/codex/bin/stop_usb_eth.sh >> "$LOG" 2>&1
  elif [ -x /data/codex/stop_usb_eth.sh ]; then
    /data/codex/stop_usb_eth.sh >> "$LOG" 2>&1
  elif [ -x /usr/sbin/stop_usb_eth.sh ]; then
    /usr/sbin/stop_usb_eth.sh >> "$LOG" 2>&1
  fi
}

monitor_wifi() {
  # Skip while recovery AP is active
  [ -f /var/run/codex-recovery-ap.pid ] && return 0

  # 1. Ensure ath0 interface exists and is up
  if ! ifconfig ath0 >/dev/null 2>&1; then
    log "ath0 missing, creating..."
    /sbin/wlanconfig ath0 create wlandev wifi0 wlanmode sta 2>/dev/null || true
    ifconfig ath0 up 2>/dev/null || true
  fi

  # 2. Ensure wpa_supplicant is running
  if ! ps | grep '[w]pa_supplicant' >/dev/null 2>&1; then
    log "wpa_supplicant died, restarting..."
    /usr/sbin/wpa_supplicant -s -iath0 -c "$WPA_CONF" >/dev/null 2>&1 &
    sleep 2
  fi

  # 3. Check WPA association status
  wpa_st=$(wpa_cli -i ath0 status 2>/dev/null | grep '^wpa_state=' | cut -d= -f2)
  if [ "$wpa_st" != "COMPLETED" ]; then
    wifi_disc_count=$(expr "$wifi_disc_count" + 1 2>/dev/null || echo 1)
    if [ "$wifi_disc_count" -eq 3 ]; then
      log "Wi-Fi state is '$wpa_st' for 30s, requesting reconnect..."
      wpa_cli -i ath0 reconnect >/dev/null 2>&1 || true
    elif [ "$wifi_disc_count" -eq 6 ]; then
      log "Wi-Fi state is '$wpa_st' for 60s, requesting reassociate..."
      wpa_cli -i ath0 reassociate >/dev/null 2>&1 || true
    elif [ "$wifi_disc_count" -ge 10 ]; then
      log "Wi-Fi link stuck for >100s, restarting wpa_supplicant..."
      killall wpa_supplicant 2>/dev/null || true
      sleep 1
      /usr/sbin/wpa_supplicant -s -iath0 -c "$WPA_CONF" >/dev/null 2>&1 &
      wifi_disc_count=0
    fi
    return 1
  fi

  wifi_disc_count=0

  # 4. Check IP address on ath0
  wifi_ip=$(ifconfig ath0 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
  if [ -z "$wifi_ip" ]; then
    dhcp_fail_count=$(expr "$dhcp_fail_count" + 1 2>/dev/null || echo 1)
    if ! pidof udhcpc >/dev/null 2>&1 || [ "$dhcp_fail_count" -ge 2 ]; then
      log "ath0 has no IP (fail count: $dhcp_fail_count), requesting DHCP lease..."
      kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || killall udhcpc 2>/dev/null || true
      rm -f /var/run/udhcpc.ath0.pid
      udhcpc -i ath0 -p /var/run/udhcpc.ath0.pid -b -t 5 -T 3 -A 15 >/dev/null 2>&1 &
      dhcp_fail_count=0
    fi
    return 1
  fi

  dhcp_fail_count=0
  cur_state=$(cat "$STATE_FILE" 2>/dev/null)
  case "$cur_state" in
    *:wifi-fallback:*) ;;
    *) echo "ath0:wifi:$wifi_ip" > "$STATE_FILE" ;;
  esac

  # 5. Check Default Gateway connectivity every ~30 seconds (every 3rd cycle)
  ping_check_cycle=$(expr "$ping_check_cycle" + 1 2>/dev/null || echo 1)
  if [ "$ping_check_cycle" -ge 3 ]; then
    ping_check_cycle=0
    gw=$(route -n 2>/dev/null | awk '$1=="0.0.0.0" && $8=="ath0" {print $2}')
    if [ -n "$gw" ]; then
      if ping "$gw" 2>/dev/null | grep -q 'is alive'; then
        gw_seen=1
        gw_fail_count=0
      else
        if [ "$gw_seen" = "1" ]; then
          gw_fail_count=$(expr "$gw_fail_count" + 1 2>/dev/null || echo 1)
          log "Gateway $gw ping failed ($gw_fail_count/5)"
          if [ "$gw_fail_count" -eq 3 ]; then
            log "Gateway unreachable for 3 checks. Restarting DHCP on ath0..."
            kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || killall udhcpc 2>/dev/null || true
            rm -f /var/run/udhcpc.ath0.pid
            udhcpc -i ath0 -p /var/run/udhcpc.ath0.pid -b -t 5 -T 3 -A 15 >/dev/null 2>&1 &
          elif [ "$gw_fail_count" -ge 5 ]; then
            log "Gateway unreachable for 5 checks. Reassociating Wi-Fi..."
            wpa_cli -i ath0 reassociate >/dev/null 2>&1 || true
            gw_fail_count=0
          fi
        fi
      fi
    else
      no_gw_count=$(expr "$no_gw_count" + 1 2>/dev/null || echo 1)
      if [ "$no_gw_count" -ge 2 ]; then
        log "ath0 has IP $wifi_ip but no default gateway. Restarting DHCP..."
        kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || killall udhcpc 2>/dev/null || true
        rm -f /var/run/udhcpc.ath0.pid
        udhcpc -i ath0 -p /var/run/udhcpc.ath0.pid -b -t 5 -T 3 -A 15 >/dev/null 2>&1 &
        no_gw_count=0
      fi
    fi
  fi
}

get_current_gw() {
  local gw
  gw=$(route -n 2>/dev/null | awk '$1=="0.0.0.0" {print $2; exit}')
  if [ -n "$gw" ] && [ "$gw" != "0.0.0.0" ]; then
    echo "$gw" > /tmp/codex_saved_gw 2>/dev/null
    echo "$gw"
    return 0
  fi
  if [ -f /tmp/codex_saved_gw ]; then
    cat /tmp/codex_saved_gw 2>/dev/null
    return 0
  fi
  if [ -n "$ETH_GATEWAY" ]; then
    echo "$ETH_GATEWAY"
    return 0
  fi
  return 1
}

flush_iface_routes() {
  local iface="$1"
  [ -z "$iface" ] && return 0
  route -n 2>/dev/null | awk -v dev="$iface" '$8==dev && $1!="0.0.0.0" {print $1, $3}' | while read -r dnet dmask; do
    [ -n "$dnet" ] && [ -n "$dmask" ] && route del -net "$dnet" netmask "$dmask" dev "$iface" 2>/dev/null || true
  done
}

send_garp() {
  local iface="$1"
  local ip="$2"
  [ -z "$iface" ] || [ -z "$ip" ] && return 0
  if [ -x /data/codex/bin/busybox ]; then
    /data/codex/bin/busybox arping -U -c 3 -I "$iface" "$ip" >/dev/null 2>&1 &
    /data/codex/bin/busybox arping -A -c 3 -I "$iface" "$ip" >/dev/null 2>&1 &
  elif [ -x /cache/bin/busybox ]; then
    /cache/bin/busybox arping -U -c 3 -I "$iface" "$ip" >/dev/null 2>&1 &
    /cache/bin/busybox arping -A -c 3 -I "$iface" "$ip" >/dev/null 2>&1 &
  fi
}

migrate_to_ethernet() {
  local eth_if="$1"
  local target_ip="$2"
  local target_mask="${3:-255.255.255.0}"
  local target_gw="$4"
  [ -z "$target_gw" ] && target_gw=$(get_current_gw)
  if [ -z "$target_gw" ] && [ -n "$target_ip" ]; then
    target_gw=$(echo "$target_ip" | sed 's/\.[0-9]*$/.1/')
  fi

  log "Migrating active connection to Ethernet ($eth_if): IP $target_ip, GW $target_gw..."

  # Stop udhcpc on ath0 if running
  if [ -f /var/run/udhcpc.ath0.pid ]; then
    kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || true
    rm -f /var/run/udhcpc.ath0.pid
  fi

  # Flush subnet routes from ath0 and unbind IP (wpa_supplicant stays associated at L2)
  flush_iface_routes "ath0"
  ifconfig ath0 0.0.0.0 2>/dev/null || true

  # Assign IP to Ethernet
  ifconfig "$eth_if" "$target_ip" netmask "$target_mask" up >> "$LOG" 2>&1

  # Explicitly configure default route
  route del default 2>/dev/null || true
  if [ -n "$target_gw" ]; then
    route add default gw "$target_gw" dev "$eth_if" >> "$LOG" 2>&1
  fi

  # Restore DNS if configured
  [ -n "$ETH_DNS" ] && echo "nameserver $ETH_DNS" > /etc/resolv.conf

  configure_arp_isolation
  send_garp "$eth_if" "$target_ip"
  echo "$eth_if:ethernet:$target_ip" > "$STATE_FILE"
  log "Migration complete: Ethernet ($eth_if) active with IP $target_ip, GW $target_gw"
}

migrate_to_wifi() {
  local eth_if="$1"
  local target_ip="$2"
  local target_mask="${3:-255.255.255.0}"
  local target_gw="$4"
  [ -z "$target_gw" ] && target_gw=$(get_current_gw)
  if [ -z "$target_gw" ] && [ -n "$target_ip" ]; then
    target_gw=$(echo "$target_ip" | sed 's/\.[0-9]*$/.1/')
  fi

  log "Migrating active connection to Wi-Fi (ath0): IP $target_ip, GW $target_gw..."

  # Flush subnet routes and clear IP from Ethernet
  if [ -n "$eth_if" ]; then
    flush_iface_routes "$eth_if"
    ifconfig "$eth_if" 0.0.0.0 2>/dev/null || true
  fi

  # Ensure ath0 interface is UP and associated
  ensure_wifi

  # Assign IP to ath0
  ifconfig ath0 "$target_ip" netmask "$target_mask" up >> "$LOG" 2>&1

  # Explicitly configure default route
  route del default 2>/dev/null || true
  if [ -n "$target_gw" ]; then
    route add default gw "$target_gw" dev ath0 >> "$LOG" 2>&1
  fi

  # Restore DNS if configured
  [ -n "$ETH_DNS" ] && echo "nameserver $ETH_DNS" > /etc/resolv.conf

  configure_arp_isolation
  send_garp "ath0" "$target_ip"
  echo "ath0:wifi-fallback:$target_ip" > "$STATE_FILE"
  log "Migration complete: Wi-Fi (ath0) active with IP $target_ip, GW $target_gw"
}

# Background watchdog: monitors Ethernet link, Wi-Fi failover, Wi-Fi health & DHCP
run_monitor() {
  wifi_disc_count=0
  dhcp_fail_count=0
  gw_fail_count=0
  no_gw_count=0
  ping_check_cycle=0
  gw_seen=0

  # Cache initial working gateway on startup
  get_current_gw >/dev/null 2>&1

  while true; do
    sleep 3
    load_config

    cur_eth=$(find_eth_interface)
    if [ -n "$cur_eth" ]; then
      # Ensure interface is up administratively for PHY auto-negotiation & carrier
      ifconfig "$cur_eth" up 2>/dev/null || true

      cfile="/sys/class/net/$cur_eth/carrier"
      carrier="0"
      [ -f "$cfile" ] && [ "$(cat "$cfile" 2>/dev/null)" = "1" ] && carrier="1"

      # Determine canonical static IP
      canonical_ip="$ETH_IP"
      if [ -z "$canonical_ip" ]; then
        canonical_ip=$(ifconfig ath0 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
      fi
      [ -z "$canonical_ip" ] && [ -f "$STATE_FILE" ] && canonical_ip=$(cat "$STATE_FILE" 2>/dev/null | awk -F: '{print $3}')

      cur_state=$(cat "$STATE_FILE" 2>/dev/null)

      if [ "$carrier" = "1" ]; then
        case "$cur_state" in
          "$cur_eth:ethernet:"*)
            # Already active on Ethernet; verify default gateway route is present
            if ! route -n 2>/dev/null | grep -q '^0\.0\.0\.0'; then
              gw=$(get_current_gw)
              [ -z "$gw" ] && [ -n "$target_ip" ] && gw=$(echo "$target_ip" | sed 's/\.[0-9]*$/.1/')
              [ -n "$gw" ] && route add default gw "$gw" dev "$cur_eth" 2>/dev/null || true
            fi
            ;;
          *)
            target_ip="${ETH_IP:-$canonical_ip}"
            if [ -n "$target_ip" ]; then
              migrate_to_ethernet "$cur_eth" "$target_ip" "${ETH_NETMASK:-255.255.255.0}" "$ETH_GATEWAY"
            else
              # Pure DHCP mode
              udhcpc -i "$cur_eth" -p /var/run/udhcpc.$cur_eth.pid -b -t 5 -T 3 >> "$LOG" 2>&1
              for i in 1 2 3 4 5; do
                e_ip=$(ifconfig "$cur_eth" 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
                [ -n "$e_ip" ] && break
                sleep 1
              done
              if [ -n "$e_ip" ]; then
                route del default dev ath0 2>/dev/null || true
                send_garp "$cur_eth" "$e_ip"
                echo "$cur_eth:ethernet:$e_ip" > "$STATE_FILE"
                log "Ethernet connected via DHCP: $cur_eth has IP $e_ip"
              fi
            fi
            ;;
        esac
      else
        # Carrier == 0 (Cable disconnected or switch powered down)
        case "$cur_state" in
          "$cur_eth:ethernet:"*)
            target_ip="${ETH_IP:-$canonical_ip}"
            if [ -n "$target_ip" ]; then
              migrate_to_wifi "$cur_eth" "$target_ip" "${ETH_NETMASK:-255.255.255.0}" "$ETH_GATEWAY"
            else
              ensure_wifi
              echo "ath0:wifi-fallback:" > "$STATE_FILE"
            fi
            ;;
          *)
            # Wi-Fi active: verify default gateway route is present
            if ! route -n 2>/dev/null | grep -q '^0\.0\.0\.0'; then
              gw=$(get_current_gw)
              [ -z "$gw" ] && [ -n "$target_ip" ] && gw=$(echo "$target_ip" | sed 's/\.[0-9]*$/.1/')
              [ -n "$gw" ] && route add default gw "$gw" dev ath0 2>/dev/null || true
            fi
            monitor_wifi
            ;;
        esac
      fi
    else
      # cur_eth not detected on bus
      cur_state=$(cat "$STATE_FILE" 2>/dev/null)
      case "$cur_state" in
        *:ethernet:*)
          log "Ethernet interface disappeared! Triggering Wi-Fi fallback..."
          ensure_wifi
          echo "ath0:wifi-fallback:" > "$STATE_FILE"
          ;;
        *)
          monitor_wifi
          ;;
      esac
    fi
  done
}

start_monitor() {
  stop_monitor
  run_monitor </dev/null >> "$LOG" 2>&1 &
  echo "$!" > /var/run/codex_net_monitor.pid
  log "Network monitor started (PID $!)"
}

stop_monitor() {
  if [ -f /var/run/codex_net_monitor.pid ]; then
    old_pid=$(cat /var/run/codex_net_monitor.pid 2>/dev/null)
    if [ -n "$old_pid" ]; then
      kill -9 "$old_pid" 2>/dev/null || true
    fi
    rm -f /var/run/codex_net_monitor.pid
  fi
}

do_boot() {
  log "Boot initialization started..."
  load_config

  # 1. Initialize Wi-Fi immediately
  ensure_wifi

  # 2. Switch USB to Host mode and load Ethernet driver immediately (no gadget delay)
  log "Starting Ethernet (USB Host mode)..."
  if start_eth; then
    log "USB Ethernet adapter initialized on boot."
  else
    log "No USB Ethernet adapter found on boot; running on Wi-Fi."
  fi

  # 3. Start dynamic interface monitor
  start_monitor
}

do_apply() {
  log "Applying network configuration live..."
  load_config
  stop_monitor

  ensure_wifi

  cur_eth=$(find_eth_interface)
  if [ -z "$cur_eth" ]; then
    start_eth || true
  fi

  start_monitor
}


case "$1" in
  boot)
    do_boot
    ;;
  apply|restart)
    do_apply
    ;;
  start_eth)
    load_config
    start_eth
    ;;
  stop_eth)
    stop_eth
    ;;
  ensure_wifi)
    ensure_wifi
    ;;
  monitor)
    run_monitor
    ;;
  start_monitor)
    start_monitor
    ;;
  stop_monitor)
    stop_monitor
    ;;
  status)
    load_config
    echo "ETH_ENABLED=$ETH_ENABLED"
    echo "ETH_FALLBACK_WIFI=$ETH_FALLBACK_WIFI"
    echo "ETH_USB_SERIAL=$ETH_USB_SERIAL"
    echo "ETH_MODE=$ETH_MODE"
    cat "$STATE_FILE" 2>/dev/null
    ;;
  *)
    echo "Usage: $0 {boot|apply|start_eth|stop_eth|ensure_wifi|monitor|start_monitor|stop_monitor|status}"
    exit 1
    ;;
esac
