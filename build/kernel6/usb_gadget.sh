#!/bin/sh
# /cache/kernel6/usb_gadget.sh up|down: the Hub's USB device side on the Linux 6.18 system
# (configfs), in place of the stock usbhid on gadgetfs.
#
#   interface 0  HID, 64-byte IN and OUT reports: the device the PC tools look for (046D:C129,
#                the stock identity), relayed to hub_util usbwifi by hub_util usbhid
#   interface 1+ CDC NCM: network to the PC (gadget0, the Hub is 192.168.7.1; rc.sh)
#
# down unbinds it from the controller (start_usb_eth.sh, before host mode).
G=/sys/kernel/config/usb_gadget/hub
UDC=ci_hdrc.0

case "$1" in
up)
    modprobe usb_f_hid && modprobe usb_f_ncm || exit 1
    grep -q ' /sys/kernel/config ' /proc/mounts || mount -t configfs none /sys/kernel/config || exit 1
    if [ ! -d $G ]; then
        mkdir -p $G && cd $G || exit 1
        echo 0x046d > idVendor
        echo 0xc129 > idProduct
        echo 0x1097 > bcdDevice
        echo 0x0200 > bcdUSB
        # composite with an NCM interface association
        echo 0xef > bDeviceClass
        echo 0x02 > bDeviceSubClass
        echo 0x01 > bDeviceProtocol
        mkdir -p strings/0x409
        echo Logitech > strings/0x409/manufacturer
        echo "Harmony Hub" > strings/0x409/product

        mkdir -p functions/hid.wifi
        echo 0 > functions/hid.wifi/protocol
        echo 0 > functions/hid.wifi/subclass
        echo 64 > functions/hid.wifi/report_length
        # vendor page FF00, 64-byte input report and 64-byte output report, no report ids
        printf '\006\000\377\011\001\241\001\025\000\046\377\000\165\010\225\100\011\001\201\002\225\100\011\001\221\002\300' \
            > functions/hid.wifi/report_desc

        mkdir -p functions/ncm.net
        # fixed addresses (from the Wi-Fi MAC, locally administered), so the PC keeps one
        # network adapter for the Hub
        mac=$(cat /sys/class/net/ath0/address 2>/dev/null)
        if [ -n "$mac" ]; then
            rest=${mac#??}
            echo "02$rest" > functions/ncm.net/dev_addr
            echo "06$rest" > functions/ncm.net/host_addr
        fi

        mkdir -p configs/c.1/strings/0x409
        echo 500 > configs/c.1/MaxPower
        echo 0x80 > configs/c.1/bmAttributes
        ln -s functions/hid.wifi configs/c.1/
        ln -s functions/ncm.net configs/c.1/
    fi
    echo $UDC > $G/UDC
    ;;
down)
    [ -d $G ] && echo "" > $G/UDC
    ;;
*)
    echo "usage: $0 up|down" >&2
    exit 2
    ;;
esac
