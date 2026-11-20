#!/bin/sh
# USB Ethernet Host mode activator
BIN_DIR="/data/harmony/bin"

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

# Detect the plugged USB adapter and load ONLY its driver (never cdc_ether with r8152). The
# modules are installed on demand: a missing one is reported in /tmp/eth_driver_state.
for i in 1 2 3 4 5; do
  [ -d /sys/bus/usb/devices/1-1 ] && break
  sleep 1
done
sh /data/harmony/usb_eth_driver.sh scan
# A USB stick (non-essential files only, see usb_storage.sh)
[ -f /data/harmony/usb_storage.sh ] && sh /data/harmony/usb_storage.sh scan || true
