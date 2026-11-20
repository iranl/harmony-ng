#!/bin/sh
# usb_eth_driver.sh - the kernel driver of the USB Ethernet adapter
#
#   scan            find the adapter (also behind a USB hub), load its modules when they are
#                   installed, and write /tmp/eth_driver_state for the web UI
#   install <name>  download the adapter's modules (update.sh fetch-module), then scan again
#
# Ethernet modules are not installed by default: scan names the one the adapter needs and
# reports it missing; the web UI then offers the download.

MODULE_DIRS="/data/harmony/modules /data/harmony/modules/usb_net /cache/modules"
TABLE=/data/harmony/usb_eth_modules.txt
STATE=/tmp/eth_driver_state
USB_DEVICES=/sys/bus/usb/devices

find_mod() {
  for _d in $MODULE_DIRS; do
    if [ -f "$_d/$1.ko" ]; then
      echo "$_d/$1.ko"
      return 0
    fi
  done
  return 1
}

mod_loaded() {
  grep -q "^$1 " /proc/modules 2>/dev/null
}

# modules_for VID:PID: the module list of the first matching table line ("-" = unsupported)
modules_for() {
  [ -f "$TABLE" ] || return 1
  while read -r _pat _mods; do
    case "$_pat" in ''|'#'*) continue ;; esac
    # $_pat is unquoted on purpose: it is a case pattern (0b95:*)
    case "$1" in
      $_pat) echo "$_mods"; return 0 ;;
    esac
  done < "$TABLE"
  return 1
}

# is_candidate DIR: a USB device that can be a network adapter: not a hub (class 09), and no
# interface of class 08 (mass storage, see usb_storage.sh) or 03 (keyboard, mouse)
is_candidate() {
  [ -f "$1/idVendor" ] || return 1
  case "$(cat "$1/bDeviceClass" 2>/dev/null)" in 09) return 1 ;; esac
  for _ic in "$1"/*:*/bInterfaceClass; do
    [ -f "$_ic" ] || continue
    case "$(cat "$_ic" 2>/dev/null)" in 08|03|09) return 1 ;; esac
  done
  return 0
}

write_state() {
  # vid pid driver missing(list) loaded(0/1) unsupported(0/1)
  {
    echo "vid=$1"
    echo "pid=$2"
    echo "driver=$3"
    echo "missing=$4"
    echo "loaded=$5"
    echo "unsupported=$6"
  } > "$STATE.new" 2>/dev/null && mv -f "$STATE.new" "$STATE" 2>/dev/null
}

# Wait briefly for the adapter's interface and turn off its checksum offload (MIPS fix)
eth_fix_new_iface() {
  for _i in 1 2 3; do
    for _ifpath in /sys/class/net/eth* /sys/class/net/usb*; do
      [ -d "$_ifpath" ] || continue
      case "${_ifpath##*/}" in
        eth0|eth1|wifi0|ath0|lo) ;;
        *)
          for _b in /data/harmony/bin/eth_fix /cache/bin/eth_fix /usr/sbin/eth_fix; do
            if [ -x "$_b" ]; then
              "$_b" "${_ifpath##*/}" >/dev/null 2>&1 || true
              break
            fi
          done
          return 0
          ;;
      esac
    done
    sleep 1
  done
}

do_scan() {
  for _dev in "$USB_DEVICES"/*; do
    # devices are 1-1, 1-1.2, ...; usb1 is the root hub and 1-1:1.0 an interface
    case "${_dev##*/}" in [0-9]*-*) ;; *) continue ;; esac
    case "${_dev##*/}" in *:*) continue ;; esac
    is_candidate "$_dev" || continue
    _vid=$(cat "$_dev/idVendor" 2>/dev/null)
    _pid=$(cat "$_dev/idProduct" 2>/dev/null)
    _mods=$(modules_for "$_vid:$_pid") || continue
    echo "[USB_ETH] Detected USB device $_vid:$_pid on ${_dev##*/}"
    if [ "$_mods" = "-" ]; then
      echo "[USB_ETH] No driver for $_vid:$_pid in this firmware"
      write_state "$_vid" "$_pid" "" "" 0 1
      return 0
    fi
    _missing=""
    _driver=""
    for _m in $_mods; do
      _driver="$_m"
      find_mod "$_m" >/dev/null || _missing="${_missing:+$_missing }$_m"
    done
    if [ -n "$_missing" ]; then
      echo "[USB_ETH] Driver $_driver for $_vid:$_pid not installed (missing: $_missing)"
      write_state "$_vid" "$_pid" "$_driver" "$_missing" 0 0
      return 0
    fi
    for _m in $_mods; do
      mod_loaded "$_m" && continue
      echo "[USB_ETH] Loading $_m.ko..."
      insmod "$(find_mod "$_m")" 2>/dev/null || true
    done
    _ok=1
    mod_loaded "$_driver" || _ok=0
    write_state "$_vid" "$_pid" "$_driver" "" "$_ok" 0
    [ "$_ok" = 1 ] && eth_fix_new_iface
    return 0
  done
  rm -f "$STATE" 2>/dev/null
  echo "[USB_ETH] No USB network adapter detected"
  return 0
}

do_install() {
  _name="$1"
  case "$_name" in
    ''|*[!a-z0-9_]*) echo "[-] Bad module name: $_name" >&2; return 1 ;;
  esac
  # the driver and the modules it needs, from the table
  _list=""
  while read -r _pat _mods; do
    case "$_pat" in ''|'#'*) continue ;; esac
    for _m in $_mods; do _last="$_m"; done
    if [ "$_last" = "$_name" ]; then _list="$_mods"; break; fi
  done < "$TABLE"
  if [ -z "$_list" ]; then
    echo "[-] $_name is not a USB Ethernet driver of this firmware" >&2
    return 1
  fi
  for _m in $_list; do
    find_mod "$_m" >/dev/null && continue
    /data/harmony/update.sh fetch-module "$_m" || return 1
  done
  do_scan
}

case "${1:-scan}" in
  scan) do_scan ;;
  install) do_install "$2" ;;
  *) echo "Usage: $0 scan|install <module>" >&2; exit 1 ;;
esac
