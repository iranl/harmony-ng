#!/bin/sh
# Hub Network Monitor - Link supervisor & failover watchdog
# Sourced or executed by network_manager.sh

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

  # Ensure stock Pro 2400 network_switch.sh is not running to avoid route/DNS conflicts
  killall -9 network_switch.sh 2>/dev/null || true

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
        echo 1 > /tmp/eth_conn_pipe 2>/dev/null || true
        case "$cur_state" in
          "$cur_eth:ethernet:"*)
            # Already active on Ethernet; ensure ath0 has no lingering IP or subnet routes
            if ifconfig ath0 2>/dev/null | grep -q 'inet addr'; then
              flush_iface_routes "ath0"
              ifconfig ath0 0.0.0.0 2>/dev/null || true
            fi
            # Verify default gateway route is present
            if ! route -n 2>/dev/null | grep -q '^0\.0\.0\.0'; then
              gw=$(get_current_gw)
              [ -z "$gw" ] && [ -n "$target_ip" ] && gw=$(echo "$target_ip" | sed 's/\.[0-9]*$/.1/')
              if [ -n "$gw" ]; then
                route add default gw "$gw" dev "$cur_eth" 2>/dev/null || true
                echo "$gw" > "/tmp/default_gw.$cur_eth" 2>/dev/null || true
              fi
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
                echo 1 > /tmp/eth_conn_pipe 2>/dev/null || true
                echo "$cur_eth:ethernet:$e_ip" > "$STATE_FILE"
                stop_recovery_ap 2>/dev/null || true
                log "Ethernet connected via DHCP: $cur_eth has IP $e_ip"
              fi
            fi
            ;;
        esac
      else
        # Carrier == 0 (Cable disconnected or switch powered down)
        echo 0 > /tmp/eth_conn_pipe 2>/dev/null || true
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
            # If ETH_FALLBACK_WIFI=1 and static IP configured, ensure ath0 has the fallback IP
            target_ip="${ETH_IP:-$canonical_ip}"
            if [ "$ETH_FALLBACK_WIFI" = "1" ] && [ -n "$target_ip" ]; then
              cur_wifi_ip=$(ifconfig ath0 2>/dev/null | grep 'inet addr' | awk -F: '{print $2}' | awk '{print $1}')
              if [ "$cur_wifi_ip" != "$target_ip" ]; then
                log "Ethernet carrier 0: migrating ath0 ($cur_wifi_ip) to fallback IP $target_ip..."
                migrate_to_wifi "$cur_eth" "$target_ip" "${ETH_NETMASK:-255.255.255.0}" "$ETH_GATEWAY"
              fi
            fi
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
  pid=$!
  echo "$pid" > /var/run/hub_net_monitor.pid
  log "Network monitor started (PID $pid)"
}

stop_monitor() {
  for pidfile in /var/run/hub_net_monitor.pid; do
    if [ -f "$pidfile" ]; then
      old_pid=$(cat "$pidfile" 2>/dev/null)
      if [ -n "$old_pid" ]; then
        kill -9 "$old_pid" 2>/dev/null || true
      fi
      rm -f "$pidfile"
    fi
  done
}
