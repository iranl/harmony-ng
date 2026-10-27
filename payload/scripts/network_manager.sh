#!/bin/sh
# Harmony Network Manager - Ethernet / USB Host & Wi-Fi supervisor
# Handles 15s USB gadget safety boot, Ethernet initialization, and Wi-Fi fallback.

PATH=/cache/bin:/data/harmony/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
CONF=/data/harmony/ethernet.conf
WPA_CONF=/etc/wpa_supplicant.ath0.conf
[ -f "$WPA_CONF" ] || WPA_CONF=/etc/wpa_supplicant.conf

LOG=/tmp/harmony-network.log
STATE_FILE=/tmp/harmony_active_net_state

set_state() {
  echo "$1" > "$STATE_FILE"
}

get_state() {
  if [ -f "$STATE_FILE" ]; then
    cat "$STATE_FILE" 2>/dev/null
  fi
}

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
load_config

configure_arp_isolation() {
  # Prevent ARP flux and broadcast loops when both eth and wifi are on same subnet
  echo 1 > /proc/sys/net/ipv4/conf/all/arp_ignore 2>/dev/null || true
  echo 1 > /proc/sys/net/ipv4/conf/default/arp_ignore 2>/dev/null || true
  echo 2 > /proc/sys/net/ipv4/conf/all/arp_announce 2>/dev/null || true
  echo 2 > /proc/sys/net/ipv4/conf/default/arp_announce 2>/dev/null || true
  echo 0 > /proc/sys/net/ipv4/conf/all/rp_filter 2>/dev/null || true
  echo 0 > /proc/sys/net/ipv4/conf/default/rp_filter 2>/dev/null || true
}

is_onboard_eth() {
  # Harmony Pro 2400 (Skin 115) has onboard physical GMAC Ethernet on eth1.
  # On standard Hub, eth1 has no physical PHY/RJ45, so carrier is always 0.
  if [ -d /sys/class/net/eth1 ]; then
    [ -e /usr/bin/network_switch.sh ] && return 0
    skin=$(cat /data/harmony/skin 2>/dev/null)
    [ "$skin" = "115" ] && return 0
    if [ -f /sys/class/net/eth1/carrier ] && [ "$(cat /sys/class/net/eth1/carrier 2>/dev/null)" = "1" ]; then
      return 0
    fi
  fi
  return 1
}

find_eth_interface() {
  # 1. Onboard physical Ethernet (Pro 2400 has physical eth1 port)
  if is_onboard_eth; then
    echo "eth1"
    return 0
  fi

  # 2. USB-based Ethernet adapters (eth0, eth2, usb0, etc.)
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

is_wifi_configured() {
  if [ -f "$WPA_CONF" ] && grep -q 'network=' "$WPA_CONF" 2>/dev/null; then
    return 0
  fi
  if [ -f /etc/wpa_supplicant.conf ] && grep -q 'network=' /etc/wpa_supplicant.conf 2>/dev/null; then
    return 0
  fi
  if [ -f /etc/wpa_supplicant.ath0.conf ] && grep -q 'network=' /etc/wpa_supplicant.ath0.conf 2>/dev/null; then
    return 0
  fi
  return 1
}

ensure_wifi() {
  load_config
  log "Ensuring Wi-Fi connection (ath0)..."
  if ! ifconfig ath0 >/dev/null 2>&1; then
    /sbin/wlanconfig ath0 create wlandev wifi0 wlanmode sta 2>/dev/null || true
  fi
  if ! ps | grep '[w]pa_supplicant' >/dev/null 2>&1; then
    /usr/sbin/wpa_supplicant -s -iath0 -c "$WPA_CONF" >/dev/null 2>&1 &
    sleep 2
  fi
  ifconfig ath0 up 2>/dev/null || true
  
  # If static IP is configured, NEVER run udhcpc
  if [ "$ETH_MODE" = "static" ] && [ -n "$ETH_IP" ]; then
    kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || killall -9 udhcpc 2>/dev/null || true
    rm -f /var/run/udhcpc.ath0.pid
    cur_state=$(get_state)
    case "$cur_state" in
      *:ethernet:*)
        # Ethernet is active; keep ath0 at L2 standby without IP to avoid IP conflict
        return 0
        ;;
      *)
        wifi_ip=$(ifconfig ath0 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
        if [ "$wifi_ip" != "$ETH_IP" ]; then
          log "Configuring static IP $ETH_IP on ath0..."
          ifconfig ath0 "$ETH_IP" netmask "${ETH_NETMASK:-255.255.255.0}" up 2>/dev/null || true
          [ -n "$ETH_GATEWAY" ] && route add default gw "$ETH_GATEWAY" dev ath0 2>/dev/null || true
          [ -n "$ETH_DNS" ] && echo "nameserver $ETH_DNS" > /etc/resolv.conf
          send_garp "ath0" "$ETH_IP"
        fi
        set_state "ath0:wifi:$ETH_IP"
        stop_recovery_ap 2>/dev/null || true
        return 0
        ;;
    esac
  fi

  # Check if ath0 already has IP
  wifi_ip=$(ifconfig ath0 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
  if [ -z "$wifi_ip" ]; then
    log "Requesting DHCP lease on ath0..."
    if [ ! -f /var/run/udhcpc.ath0.pid ] || ! kill -0 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null; then
      udhcpc -i ath0 -p /var/run/udhcpc.ath0.pid -b -t 5 -T 3 -A 15 >/dev/null 2>&1 &
    fi
    for i in 1 2 3 4 5 6 7 8 9 10; do
      wifi_ip=$(ifconfig ath0 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
      [ -n "$wifi_ip" ] && break
      sleep 1
    done
  fi

  if [ -n "$wifi_ip" ]; then
    log "Wi-Fi connected successfully: ath0 has IP $wifi_ip"
    set_state "ath0:wifi:$wifi_ip"
    stop_recovery_ap 2>/dev/null || true
    return 0
  else
    log "Wi-Fi IP lease pending in background"
    return 1
  fi
}

start_eth() {
  eth_if=$(find_eth_interface)
  if [ "$eth_if" = "eth1" ] && is_onboard_eth; then
    log "Found onboard physical Ethernet interface: eth1"
    configure_arp_isolation
    ifconfig "$eth_if" up >> "$LOG" 2>&1
  else
    log "Switching USB to host mode and loading drivers..."
    START_USB_SCRIPT=""
    for s in /data/harmony/bin/start_usb_eth.sh /data/harmony/start_usb_eth.sh /usr/sbin/start_usb_eth.sh; do
      if [ -x "$s" ]; then START_USB_SCRIPT="$s"; break; fi
    done
    if [ -n "$START_USB_SCRIPT" ]; then
      "$START_USB_SCRIPT" >> "$LOG" 2>&1
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
  fi

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
    echo 0 > /tmp/eth_conn_pipe 2>/dev/null || true
    return 2
  fi

  echo 1 > /tmp/eth_conn_pipe 2>/dev/null || true

  apply_eth_fix() {
    local dev="$1"
    [ -z "$dev" ] && return 0
    FIX_BIN=""
    for b in /data/harmony/bin/eth_fix /cache/bin/eth_fix /usr/sbin/eth_fix; do
      if [ -x "$b" ]; then FIX_BIN="$b"; break; fi
    done
    [ -n "$FIX_BIN" ] && "$FIX_BIN" "$dev" >> "$LOG" 2>&1 || true
  }

  apply_eth_fix "$eth_if"

  if [ "$ETH_MODE" = "static" ] && [ -n "$ETH_IP" ]; then
    log "Configuring static IP $ETH_IP on $eth_if..."
    ifconfig "$eth_if" "$ETH_IP" netmask "${ETH_NETMASK:-255.255.255.0}" up >> "$LOG" 2>&1
    apply_eth_fix "$eth_if"
    route del default dev "$eth_if" 2>/dev/null || true
    route del default dev ath0 2>/dev/null || true
    if [ -n "$ETH_GATEWAY" ]; then
      route add default gw "$ETH_GATEWAY" dev "$eth_if" >> "$LOG" 2>&1
    fi
    [ -n "$ETH_DNS" ] && echo "nameserver $ETH_DNS" > /etc/resolv.conf
    send_garp "$eth_if" "$ETH_IP"
    set_state "$eth_if:ethernet:$ETH_IP"
    stop_recovery_ap 2>/dev/null || true
    log "Ethernet configured statically: $eth_if has IP $ETH_IP"
    return 0
  else
    # Pure DHCP mode
    log "Requesting DHCP lease on $eth_if..."
    kill -9 $(cat /var/run/udhcpc.$eth_if.pid 2>/dev/null) 2>/dev/null || true
    udhcpc -i "$eth_if" -p /var/run/udhcpc.$eth_if.pid -b -t 5 -T 3 >> "$LOG" 2>&1
    for i in 1 2 3 4 5 6 7 8; do
      eth_ip=$(ifconfig "$eth_if" 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
      [ -n "$eth_ip" ] && break
      sleep 1
    done
    if [ -n "$eth_ip" ]; then
      apply_eth_fix "$eth_if"
      route del default dev ath0 2>/dev/null || true
      send_garp "$eth_if" "$eth_ip"
      set_state "$eth_if:ethernet:$eth_ip"
      stop_recovery_ap 2>/dev/null || true
      log "Ethernet connected via DHCP: $eth_if has IP $eth_ip"
      return 0
    else
      log "DHCP failed to obtain IP on $eth_if"
      return 3
    fi
  fi
}

stop_eth() {
  log "Stopping Ethernet..."
  echo 0 > /tmp/eth_conn_pipe 2>/dev/null || true
  if is_onboard_eth; then
    ifconfig eth1 0.0.0.0 down 2>/dev/null || true
  else
    STOP_USB_SCRIPT=""
    for s in /data/harmony/bin/stop_usb_eth.sh /data/harmony/stop_usb_eth.sh /usr/sbin/stop_usb_eth.sh; do
      if [ -x "$s" ]; then STOP_USB_SCRIPT="$s"; break; fi
    done
    [ -n "$STOP_USB_SCRIPT" ] && "$STOP_USB_SCRIPT" >> "$LOG" 2>&1 || true
  fi
}

monitor_wifi() {
  # Skip while recovery AP is active
  [ -f /var/run/hub-recovery-ap.pid ] && return 0

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
  cur_state=$(cat "$STATE_FILE" 2>/dev/null)
  if [ -z "$wifi_ip" ]; then
    target_ip="${ETH_IP:-$canonical_ip}"
    if [ "$ETH_MODE" = "static" ] && [ -n "$target_ip" ]; then
      log "Restoring static IP $target_ip on ath0..."
      kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || killall -9 udhcpc 2>/dev/null || true
      rm -f /var/run/udhcpc.ath0.pid
      ifconfig ath0 "$target_ip" netmask "${ETH_NETMASK:-255.255.255.0}" up 2>/dev/null || true
      [ -n "$ETH_GATEWAY" ] && route add default gw "$ETH_GATEWAY" dev ath0 2>/dev/null || true
      send_garp "ath0" "$target_ip"
      return 0
    fi
    case "$cur_state" in
      *:wifi-fallback:*)
        if [ -n "$target_ip" ]; then
          log "Restoring fallback static IP $target_ip on ath0..."
          ifconfig ath0 "$target_ip" netmask "${ETH_NETMASK:-255.255.255.0}" up 2>/dev/null || true
          send_garp "ath0" "$target_ip"
          return 0
        fi
        ;;
    esac
    dhcp_fail_count=$(expr "$dhcp_fail_count" + 1 2>/dev/null || echo 1)
    if ! pidof udhcpc >/dev/null 2>&1 || [ "$dhcp_fail_count" -ge 2 ]; then
      log "ath0 has no IP (fail count: $dhcp_fail_count), requesting DHCP lease..."
      kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || killall -9 udhcpc 2>/dev/null || true
      rm -f /var/run/udhcpc.ath0.pid
      udhcpc -i ath0 -p /var/run/udhcpc.ath0.pid -b -t 5 -T 3 -A 15 >/dev/null 2>&1 &
      dhcp_fail_count=0
    fi
    return 1
  fi

  dhcp_fail_count=0
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
      if ping -c 1 "$gw" >/dev/null 2>&1; then
        gw_seen=1
        gw_fail_count=0
      else
        case "$cur_state" in
          *:wifi-fallback:*)
            # In static fallback mode, gateway might be unreachable (switch off); do not restart DHCP or reassociate
            ;;
          *)
            if [ "$gw_seen" = "1" ]; then
              gw_fail_count=$(expr "$gw_fail_count" + 1 2>/dev/null || echo 1)
              log "Gateway $gw ping failed ($gw_fail_count/5)"
              if [ "$gw_fail_count" -eq 3 ]; then
                if [ "$ETH_MODE" = "static" ]; then
                  log "Gateway unreachable. Static IP mode: preserving IP on ath0..."
                  [ -n "$ETH_GATEWAY" ] && route add default gw "$ETH_GATEWAY" dev ath0 2>/dev/null || true
                else
                  log "Gateway unreachable for 3 checks. Restarting DHCP on ath0..."
                  kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || killall -9 udhcpc 2>/dev/null || true
                  rm -f /var/run/udhcpc.ath0.pid
                  udhcpc -i ath0 -p /var/run/udhcpc.ath0.pid -b -t 5 -T 3 -A 15 >/dev/null 2>&1 &
                fi
              elif [ "$gw_fail_count" -ge 5 ]; then
                log "Gateway unreachable for 5 checks. Reassociating Wi-Fi..."
                wpa_cli -i ath0 reassociate >/dev/null 2>&1 || true
                gw_fail_count=0
              fi
            fi
            ;;
        esac
      fi
    else
      case "$cur_state" in
        *:wifi-fallback:*)
          gw=$(get_current_gw)
          [ -z "$gw" ] && [ -n "$target_ip" ] && gw=$(echo "$target_ip" | sed 's/\.[0-9]*$/.1/')
          [ -n "$gw" ] && route add default gw "$gw" dev ath0 2>/dev/null || true
          ;;
        *)
          no_gw_count=$(expr "$no_gw_count" + 1 2>/dev/null || echo 1)
          if [ "$no_gw_count" -ge 2 ]; then
            if [ "$ETH_MODE" = "static" ] && [ -n "$ETH_GATEWAY" ]; then
              log "Restoring default gateway $ETH_GATEWAY on ath0..."
              route add default gw "$ETH_GATEWAY" dev ath0 2>/dev/null || true
            else
              log "ath0 has IP $wifi_ip but no default gateway. Restarting DHCP..."
              kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || killall -9 udhcpc 2>/dev/null || true
              rm -f /var/run/udhcpc.ath0.pid
              udhcpc -i ath0 -p /var/run/udhcpc.ath0.pid -b -t 5 -T 3 -A 15 >/dev/null 2>&1 &
            fi
            no_gw_count=0
          fi
          ;;
      esac
    fi
  fi
}

get_current_gw() {
  local gw
  gw=$(route -n 2>/dev/null | awk '$1=="0.0.0.0" {print $2; exit}')
  if [ -n "$gw" ] && [ "$gw" != "0.0.0.0" ]; then
    echo "$gw" > /tmp/harmony_saved_gw 2>/dev/null
    echo "$gw"
    return 0
  fi
  if [ -f /tmp/harmony_saved_gw ]; then
    cat /tmp/harmony_saved_gw 2>/dev/null
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
  ARP_BB=""
  for b in /data/harmony/bin/busybox /cache/bin/busybox; do
    if [ -x "$b" ]; then ARP_BB="$b"; break; fi
  done
  if [ -n "$ARP_BB" ]; then
    "$ARP_BB" arping -U -c 3 -I "$iface" "$ip" >/dev/null 2>&1 &
    "$ARP_BB" arping -A -c 3 -I "$iface" "$ip" >/dev/null 2>&1 &
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

  # Disable hardware checksum & segmentation offload on MIPS to prevent corrupted TCP checksums
  FIX_BIN=""
  for b in /data/harmony/bin/eth_fix /cache/bin/eth_fix /usr/sbin/eth_fix; do
    if [ -x "$b" ]; then FIX_BIN="$b"; break; fi
  done
  [ -n "$FIX_BIN" ] && "$FIX_BIN" "$eth_if" >> "$LOG" 2>&1 || true

  # Explicitly configure default route
  while route del default 2>/dev/null; do :; done
  if [ -n "$target_gw" ]; then
    route add default gw "$target_gw" dev "$eth_if" >> "$LOG" 2>&1
    echo "$target_gw" > "/tmp/default_gw.$eth_if" 2>/dev/null || true
  fi

  # Restore DNS if configured
  [ -n "$ETH_DNS" ] && echo "nameserver $ETH_DNS" > /etc/resolv.conf
  [ -f /etc/resolv.conf ] && cp /etc/resolv.conf "/tmp/resolv.conf.$eth_if" 2>/dev/null || true

  configure_arp_isolation
  send_garp "$eth_if" "$target_ip"
  echo 1 > /tmp/eth_conn_pipe 2>/dev/null || true
  set_state "$eth_if:ethernet:$target_ip"
  stop_recovery_ap 2>/dev/null || true
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

  # Stop udhcpc on ath0 if running
  if [ -f /var/run/udhcpc.ath0.pid ] || [ "$ETH_MODE" = "static" ]; then
    kill -9 $(cat /var/run/udhcpc.ath0.pid 2>/dev/null) 2>/dev/null || true
    killall -9 udhcpc 2>/dev/null || true
    rm -f /var/run/udhcpc.ath0.pid /var/run/udhcpc.*.pid
  fi

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
  while route del default 2>/dev/null; do :; done
  if [ -n "$target_gw" ]; then
    route add default gw "$target_gw" dev ath0 >> "$LOG" 2>&1
    echo "$target_gw" > "/tmp/default_gw.ath0" 2>/dev/null || true
  fi

  # Restore DNS if configured
  [ -n "$ETH_DNS" ] && echo "nameserver $ETH_DNS" > /etc/resolv.conf
  [ -f /etc/resolv.conf ] && cp /etc/resolv.conf "/tmp/resolv.conf.ath0" 2>/dev/null || true

  configure_arp_isolation
  send_garp "ath0" "$target_ip"
  echo 0 > /tmp/eth_conn_pipe 2>/dev/null || true
  set_state "ath0:wifi-fallback:$target_ip"
  log "Migration complete: Wi-Fi (ath0) active with IP $target_ip, GW $target_gw"
}

# Source network monitor functions
NET_MON_SCRIPT=""
for s in /data/harmony/scripts/network_monitor.sh /data/harmony/network_monitor.sh "$(dirname "$0")/network_monitor.sh" /payload/scripts/network_monitor.sh; do
  if [ -f "$s" ]; then NET_MON_SCRIPT="$s"; break; fi
done
[ -n "$NET_MON_SCRIPT" ] && . "$NET_MON_SCRIPT"

start_recovery_ap() {
  log "Starting recovery captive portal AP..."
  for s in /data/harmony/scripts/recovery_ap.sh /data/harmony/recovery_ap.sh /data/harmony/bin/recovery_ap.sh; do
    if [ -x "$s" ]; then
      "$s" start >> "$LOG" 2>&1 &
      return 0
    fi
  done
  log "Warning: recovery_ap.sh not found!"
  return 1
}

stop_recovery_ap() {
  if [ -f /var/run/hub-recovery-ap.pid ]; then
    log "Stopping recovery AP..."
    for s in /data/harmony/scripts/recovery_ap.sh /data/harmony/recovery_ap.sh /data/harmony/bin/recovery_ap.sh; do
      if [ -x "$s" ]; then
        "$s" stop >> "$LOG" 2>&1
        return 0
      fi
    done
  fi
}

do_boot() {
  log "Boot initialization started..."
  load_config

  # Kill stock Pro 2400 network_switch.sh to avoid routing conflicts
  killall -9 network_switch.sh 2>/dev/null || true

  # Kill any rogue udhcpc started by stock ifup -a
  if [ "$ETH_MODE" = "static" ]; then
    killall -9 udhcpc 2>/dev/null || true
    rm -f /var/run/udhcpc.*.pid
  fi

  # 1. Initialize Wi-Fi immediately
  has_wifi=0
  if ensure_wifi; then
    has_wifi=1
  fi

  # 2. Initialize Ethernet (Onboard eth1 on Pro 2400, or USB Host mode on standard Hub)
  has_eth=0
  if is_onboard_eth; then
    log "Starting Onboard Ethernet (eth1)..."
  else
    log "Starting Ethernet (USB Host mode)..."
  fi

  if start_eth; then
    if is_onboard_eth; then
      log "Onboard Ethernet interface eth1 initialized on boot."
    else
      log "USB Ethernet adapter initialized on boot."
    fi
    has_eth=1
    stop_recovery_ap 2>/dev/null || true
  else
    log "No active Ethernet link on boot; evaluating Wi-Fi fallback."
    cur_eth=$(find_eth_interface)
    if [ "$ETH_FALLBACK_WIFI" = "1" ] && [ -n "$ETH_IP" ]; then
      log "ETH_FALLBACK_WIFI active: assigning static fallback IP $ETH_IP to ath0..."
      migrate_to_wifi "$cur_eth" "$ETH_IP" "${ETH_NETMASK:-255.255.255.0}" "$ETH_GATEWAY"
      has_wifi=1
    fi
  fi

  # If both Wi-Fi and Ethernet are offline AND Wi-Fi has no credentials, activate recovery AP
  if [ "$has_wifi" -eq 0 ] && [ "$has_eth" -eq 0 ]; then
    if ! is_wifi_configured; then
      log "No active network link and no Wi-Fi credentials found on boot. Spawning recovery AP captive portal..."
      start_recovery_ap
    else
      log "Wi-Fi is configured (association/DHCP pending). Skipping recovery AP; dynamic monitor will maintain link."
    fi
  fi

  # 3. Start dynamic interface monitor
  start_monitor
}

do_apply() {
  log "Applying network configuration live..."
  load_config
  stop_monitor

  if [ "$ETH_MODE" = "static" ]; then
    killall -9 udhcpc 2>/dev/null || true
    rm -f /var/run/udhcpc.*.pid
  fi

  cur_eth=$(find_eth_interface)
  if [ -n "$cur_eth" ]; then
    cfile="/sys/class/net/$cur_eth/carrier"
    if [ -f "$cfile" ] && [ "$(cat "$cfile" 2>/dev/null)" = "1" ]; then
      start_eth || true
    else
      ensure_wifi
    fi
  else
    ensure_wifi
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
  start_recovery_ap)
    start_recovery_ap
    ;;
  stop_recovery_ap)
    stop_recovery_ap
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
