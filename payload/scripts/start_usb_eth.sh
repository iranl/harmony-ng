#!/bin/sh
# USB Ethernet Host mode activator
MODULES_DIR="/data/harmony/modules"
BIN_DIR="/data/harmony/bin"
# Linux 6.18 Hub system (build/kernel6): ChipIdea USB controller, role switched in sysfs
CI_ROLE=/sys/bus/platform/devices/1b000000.usb/ci_hdrc.0/role
KERNEL6=0
# its driver unloaded (by hand): load it again, never the stock-kernel register_ehci on 6.x
case "$(uname -r)" in
  6.*) [ -e "$CI_ROLE" ] || { modprobe ci_hdrc_usb2 2>/dev/null; sleep 1; } ;;
esac
[ -e "$CI_ROLE" ] && KERNEL6=1

load_mod() {
  mod="$1"
  if [ "$KERNEL6" = 1 ]; then
    # its own modules (/lib/modules/<release>), with what they need
    modprobe "${mod%.ko}" 2>/dev/null || true
  elif [ -f "$MODULES_DIR/$mod" ]; then
    insmod "$MODULES_DIR/$mod" 2>/dev/null || true
  elif [ -f "$MODULES_DIR/usb_net/$mod" ]; then
    insmod "$MODULES_DIR/usb_net/$mod" 2>/dev/null || true
  elif [ -f "/cache/modules/$mod" ]; then
    insmod "/cache/modules/$mod" 2>/dev/null || true
  fi
}

if [ "$KERNEL6" = 1 ]; then
  echo "[USB_ETH] Switching USB controller to Host mode (ChipIdea role)..."
  echo host > /tmp/harmony_usb_port_mode 2>/dev/null || true
  # the USB device side (HID + network gadget, udhcpd, hub_util usbhid) goes; usb_gadget.sh is
  # in the root file system, or on /cache for its fallback boot
  for _g in /usr/lib/kernel6/usb_gadget.sh /cache/kernel6/usb_gadget.sh; do
    [ -f "$_g" ] && { sh "$_g" stop 2>/dev/null; break; }
  done
  echo host > "$CI_ROLE" || echo "[USB_ETH] Error: cannot set the USB role to host" >&2
else
# Preload usbcore
insmod /lib/modules/2.6.31-g89d565c/kernel/drivers/usb/core/usbcore.ko 2>/dev/null || true

echo "[USB_ETH] Switching USB controller to Host mode..."
# Written first: from now on init.sh does not start usbhid again (see its 5.1)
echo host > /tmp/harmony_usb_port_mode 2>/dev/null || true
killall -9 usbgadget usbhid 2>/dev/null || true
# usbhid has the gadget open (it brings it up as soon as hub_usbwifi listens): a killed
# usbhid may still hold it for a moment, and gadgetfs cannot go while it does
_i=0
while [ "$_i" -lt 5 ] && pidof usbhid >/dev/null 2>&1; do
  sleep 1
  _i=$(expr "$_i" + 1)
done
for _try in 1 2 3; do
  umount /dev/gadget 2>/dev/null || true
  rmmod gadgetfs 2>/dev/null || true
  rmmod ath_udc 2>/dev/null || true
  grep -q '^ath_udc ' /proc/modules 2>/dev/null || break
  sleep 1
done
if grep -q '^ath_udc ' /proc/modules 2>/dev/null; then
  echo "[USB_ETH] Warning: the USB device driver (ath_udc) could not be unloaded; host mode may not work until a restart"
fi

# Ensure bit 0 is set on usbctrlr1 so ehci_reset runs the Atheros PHY & host reset sequence
[ -x "$BIN_DIR/set_phy_host" ] && "$BIN_DIR/set_phy_host" s >/dev/null 2>&1 || true

# Load official unpatched EHCI host driver (high-speed 480Mbps support)
insmod /lib/modules/2.6.31-g89d565c/kernel/drivers/usb/host/ehci-hcd.ko 2>/dev/null || true

# Register dormant platform device ar7240-ehci.0 via sys_call_table ONLY if not already bound.
# Never twice in one boot: when ehci-hcd did not load, the device is registered but has no
# driver, and the "driver" test alone let every later start (each "init.sh restart")
# register the same static device again, which stop_usb_eth.sh notes can crash. The marker is
# in /tmp (RAM), so a reboot clears it; a registration that left no device can be retried.
REG_MARKER=/tmp/harmony_ehci_registered
if [ ! -d /sys/devices/platform/ar7240-ehci.0/driver ] && \
   ! { [ -f "$REG_MARKER" ] && [ -d /sys/devices/platform/ar7240-ehci.0 ]; }; then
  REG_BIN=""
  for b in "$BIN_DIR/register_ehci" /cache/bin/register_ehci /usr/sbin/register_ehci; do
    if [ -x "$b" ]; then REG_BIN="$b"; break; fi
  done
  if [ -n "$REG_BIN" ]; then
    touch "$REG_MARKER" 2>/dev/null
    "$REG_BIN"
    echo "[USB_ETH] register_ehci exit code $?"
  else
    echo "[USB_ETH] Error: register_ehci not found!" >&2
  fi
fi
fi  # stock kernel

# Detect plugged USB device and load ONLY its specific driver (never load cdc_ether with r8152)
for i in 1 2 3 4 5; do
  [ -d /sys/bus/usb/devices/1-1 ] && break
  sleep 1
done

# A network adapter at full or low speed (USB 1.1 adapter, or one behind a USB 1.1 hub, as in
# some OTG cables with a hub) goes through the controller's transaction translator. The Hub
# froze (only a power cycle helped) as soon as a driver waited for received data from such a
# device, but that was seen with modules of a shifted struct sk_buff layout; until it is tested
# again with the rebuilt modules, the guard stays. A loaded driver binds every matching device on the bus, so no network driver is loaded while
# such a device is present. Hubs (class 09), USB sticks (08) and keyboards or mice (HID, 03)
# do not count, but only when every interface of the device is one of these: a composite
# device with storage and CDC Ethernet does.
slow_usb_device() {
  for _dev in /sys/bus/usb/devices/*; do
    case "${_dev##*/}" in [0-9]*-*) ;; *) continue ;; esac
    case "${_dev##*/}" in *:*) continue ;; esac
    case "$(cat "$_dev/speed" 2>/dev/null)" in 12|1.5) ;; *) continue ;; esac
    [ "$(cat "$_dev/bDeviceClass" 2>/dev/null)" = "09" ] && continue
    _other=1
    for _ic in "$_dev"/*:*/bInterfaceClass; do
      [ -f "$_ic" ] || continue
      case "$(cat "$_ic" 2>/dev/null)" in
        03|08|09) [ "$_other" = 1 ] && _other=0 ;;
        *) _other=2 ;;
      esac
    done
    [ "$_other" = 0 ] && continue
    echo "${_dev##*/} $(cat "$_dev/idVendor" 2>/dev/null):$(cat "$_dev/idProduct" 2>/dev/null)"
    return 0
  done
  return 1
}

# The network adapter: 1-1 itself, or behind a USB hub (1-1.2, 1-1.3.1, ...) next to a USB stick.
# Hubs, sticks and keyboards or mice (every interface 03, 08 or 09) are skipped, as above.
# A USB Wi-Fi adapter (Linux 6.18, build/kernel6/rc.sh: USB_WIFI_ADAPTER), never an Ethernet
# one: an interface with a wireless network device, or an id one of the Wi-Fi drivers names in
# modules.alias (also before its driver is loaded)
usb_wifi_dev() {
  ls -d "$1"/*:*/net/*/phy80211 >/dev/null 2>&1 && return 0
  [ "$KERNEL6" = 1 ] || return 1
  _v=$(cat "$1/idVendor" 2>/dev/null)
  _p=$(cat "$1/idProduct" 2>/dev/null)
  [ -n "$_v" ] && [ -n "$_p" ] || return 1
  grep -i "^alias usb:v${_v}p${_p}d" "/lib/modules/$(uname -r)/modules.alias" 2>/dev/null |
    grep -q -e ' mt76x[02]u$' -e ' rtw88_88[0-9][0-9][abc]u$'
}

net_usb_device() {
  for _dev in /sys/bus/usb/devices/*; do
    case "${_dev##*/}" in [0-9]*-*) ;; *) continue ;; esac
    case "${_dev##*/}" in *:*) continue ;; esac
    [ "$(cat "$_dev/bDeviceClass" 2>/dev/null)" = "09" ] && continue
    usb_wifi_dev "$_dev" && continue
    _other=1
    for _ic in "$_dev"/*:*/bInterfaceClass; do
      [ -f "$_ic" ] || continue
      case "$(cat "$_ic" 2>/dev/null)" in
        03|08|09) [ "$_other" = 1 ] && _other=0 ;;
        *) _other=2 ;;
      esac
    done
    [ "$_other" = 0 ] && continue
    echo "$_dev"
    return 0
  done
  return 1
}

# devices behind a hub show up a moment after the hub
net_dev=""
for i in 1 2 3; do
  net_dev=$(net_usb_device) && break
  sleep 1
done

if slow=$(slow_usb_device); then
  echo "[USB_ETH] USB device $slow runs at full or low speed (USB 1.1): no network driver is loaded (such an adapter froze the Hub in a test). Use a USB 2.0 (high-speed) adapter, hub or OTG cable."
elif [ -n "$net_dev" ] && [ -f "$net_dev/idVendor" ]; then
  vid=$(cat "$net_dev/idVendor" 2>/dev/null)
  pid=$(cat "$net_dev/idProduct" 2>/dev/null)
  echo "[USB_ETH] Detected USB device $vid:$pid"
  case "$vid:$pid" in
    0bda:8152|0bda:8153|2357:0601)
      echo "[USB_ETH] Loading Realtek native driver (r8152.ko)..."
      load_mod r8152.ko
      ;;
    0b95:1790|0b95:178a)
      echo "[USB_ETH] Loading ASIX AX88179 driver..."
      load_mod ax88179_178a.ko
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
      load_mod cdc_ncm.ko
      ;;
  esac
else
  echo "[USB_ETH] No USB network adapter detected (only hubs, sticks or input devices, or nothing)"
fi

# Wait briefly for network interface creation and apply MIPS checksum/TSO fix
# Linux 6.18 on a plain Hub loads no onboard Ethernet driver, so a USB adapter can get eth0 or
# eth1, the names network_manager.sh keeps for the onboard ports: it becomes eth2 or eth3
if [ "$KERNEL6" = 1 ]; then
  for i in 1 2 3 4 5; do
    for _n in eth0 eth1; do
      case "$(readlink -f "/sys/class/net/$_n/device" 2>/dev/null)" in
        */usb*) ip link set "$_n" name "eth$((${_n#eth} + 2))" 2>/dev/null ;;
      esac
    done
    ls /sys/class/net/eth[2-9] >/dev/null 2>&1 && break
    sleep 1
  done
fi

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
