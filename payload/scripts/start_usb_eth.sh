#!/bin/sh
# USB Ethernet Host mode activator
MODULES_DIR="/data/harmony/modules"
BIN_DIR="/data/harmony/bin"

load_mod() {
  mod="$1"
  if [ -f "$MODULES_DIR/$mod" ]; then
    insmod "$MODULES_DIR/$mod" 2>/dev/null || true
  elif [ -f "$MODULES_DIR/usb_net/$mod" ]; then
    insmod "$MODULES_DIR/usb_net/$mod" 2>/dev/null || true
  elif [ -f "/cache/modules/$mod" ]; then
    insmod "/cache/modules/$mod" 2>/dev/null || true
  fi
}

# Preload usbcore
insmod /lib/modules/2.6.31-g89d565c/kernel/drivers/usb/core/usbcore.ko 2>/dev/null || true

echo "[USB_ETH] Switching USB controller to Host mode..."
killall -9 usbgadget usbhid 2>/dev/null || true
umount /dev/gadget 2>/dev/null || true
rmmod gadgetfs 2>/dev/null || true
rmmod ath_udc 2>/dev/null || true

# Ensure bit 0 is set on usbctrlr1 so ehci_reset runs the Atheros PHY & host reset sequence
[ -x "$BIN_DIR/set_phy_host" ] && "$BIN_DIR/set_phy_host" s >/dev/null 2>&1 || true

# Load official unpatched EHCI host driver (high-speed 480Mbps support)
insmod /lib/modules/2.6.31-g89d565c/kernel/drivers/usb/host/ehci-hcd.ko 2>/dev/null || true

# Register dormant platform device ar7240-ehci.0 via sys_call_table ONLY if not already bound
if [ ! -d /sys/devices/platform/ar7240-ehci.0/driver ]; then
  REG_BIN=""
  for b in "$BIN_DIR/register_ehci" /cache/bin/register_ehci /usr/sbin/register_ehci; do
    if [ -x "$b" ]; then REG_BIN="$b"; break; fi
  done
  if [ -n "$REG_BIN" ]; then
    "$REG_BIN"
  else
    echo "[USB_ETH] Error: register_ehci not found!" >&2
  fi
fi

# Detect plugged USB device and load ONLY its specific driver (never load cdc_ether with r8152)
for i in 1 2 3 4 5; do
  [ -d /sys/bus/usb/devices/1-1 ] && break
  sleep 1
done

if [ -f /sys/bus/usb/devices/1-1/idVendor ]; then
  vid=$(cat /sys/bus/usb/devices/1-1/idVendor 2>/dev/null)
  pid=$(cat /sys/bus/usb/devices/1-1/idProduct 2>/dev/null)
  echo "[USB_ETH] Detected USB device $vid:$pid"
  case "$vid:$pid" in
    0bda:8152|0bda:8153)
      echo "[USB_ETH] Loading Realtek native driver (r8152.ko)..."
      load_mod r8152.ko
      ;;
    0b95:*|077b:2226|0846:1040)
      echo "[USB_ETH] Loading ASIX driver..."
      load_mod usbnet.ko
      load_mod asix.ko
      ;;
    0424:*)
      echo "[USB_ETH] Loading SMSC driver..."
      load_mod usbnet.ko
      load_mod smsc95xx.ko
      ;;
    0a46:*)
      echo "[USB_ETH] Loading DM9601 driver..."
      load_mod usbnet.ko
      load_mod dm9601.ko
      ;;
    0bda:8150)
      echo "[USB_ETH] Loading RTL8150 driver..."
      load_mod rtl8150.ko
      ;;
    *)
      echo "[USB_ETH] Loading generic CDC Ethernet driver..."
      load_mod usbnet.ko
      load_mod cdc_ether.ko
      ;;
  esac
else
  echo "[USB_ETH] No USB device detected yet on port 1-1"
fi

# Wait briefly for network interface creation and apply MIPS checksum/TSO fix
for i in 1 2 3; do
  for ifpath in /sys/class/net/eth* /sys/class/net/usb*; do
    [ -d "$ifpath" ] || continue
    ifname="${ifpath##*/}"
    case "$ifname" in
      eth0|eth1|wifi0|ath0|lo) ;;
      *)
        FIX_BIN=""
        for b in "$BIN_DIR/eth_fix" /cache/bin/eth_fix /usr/sbin/eth_fix; do
          if [ -x "$b" ]; then FIX_BIN="$b"; break; fi
        done
        [ -n "$FIX_BIN" ] && "$FIX_BIN" "$ifname" >/dev/null 2>&1 || true
        break 2
        ;;
    esac
  done
  sleep 1
done
