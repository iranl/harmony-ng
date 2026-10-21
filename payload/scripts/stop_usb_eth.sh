#!/bin/sh
# Revert USB Ethernet back to stock gadget mode
# Keep EHCI host controller alive to prevent re-registration crash
for dev in /sys/class/net/eth* /sys/class/net/usb*; do
  [ -d "$dev" ] || continue
  name="${dev##*/}"
  if [ "$name" != "eth0" ] && [ "$name" != "eth1" ]; then
    ifconfig "$name" 0.0.0.0 down 2>/dev/null || true
  fi
done
echo "[USB_ETH] Stopped USB ethernet interfaces."

