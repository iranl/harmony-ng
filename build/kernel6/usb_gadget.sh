#!/bin/sh
# /cache/kernel6/usb_gadget.sh up|down: the Hub's USB device side on the Linux 6.18 system
# (configfs), in place of the stock usbhid on gadgetfs.
#
#   interface 0  HID, 64-byte IN and OUT reports: the device the PC tools look for (046D:C129,
#                the stock identity), relayed to hub_util usbwifi by hub_util usbhid
#   interface 1+ CDC NCM: network to the PC (gadget0, the Hub is 192.168.7.1; rc.sh)
#
# down unbinds it from the controller and removes it (up creates it again): an unbound NCM
# function kept its network interface, so gadget0 stayed in host mode.
#
# start  the whole device side: USB role gadget, up, usb0 renamed gadget0 (network_manager.sh
#        takes usb* for an Ethernet adapter) as 192.168.7.1 with a DHCP server giving the PC
#        192.168.7.2, and hub_util usbhid (rc.sh at boot; network_manager.sh when host mode
#        found no Ethernet adapter)
# stop   all of it down (start_usb_eth.sh, before host mode)
G=/sys/kernel/config/usb_gadget/hub
UDC=ci_hdrc.0
ROLE=/sys/bus/platform/devices/1b000000.usb/ci_hdrc.0/role

case "$1" in
start)
    echo gadget > $ROLE 2>/dev/null || echo "usb_gadget: cannot set the USB role to gadget" >&2
    sh "$0" up || exit 1
    i=0
    while [ ! -e /sys/class/net/usb0 ] && [ ! -e /sys/class/net/gadget0 ] && [ $i -lt 15 ]; do sleep 0.2; i=$((i + 1)); done
    [ -e /sys/class/net/usb0 ] && ip link set usb0 name gadget0
    ifconfig gadget0 192.168.7.1 netmask 255.255.255.0 up || exit 1
    printf 'start 192.168.7.2\nend 192.168.7.2\ninterface gadget0\nlease_file /tmp/udhcpd.leases\npidfile /var/run/udhcpd.gadget0.pid\noption subnet 255.255.255.0\n' > /tmp/udhcpd.conf
    touch /tmp/udhcpd.leases
    kill "$(cat /var/run/udhcpd.gadget0.pid 2>/dev/null)" 2>/dev/null
    udhcpd /tmp/udhcpd.conf
    for u in ${HUB_UTIL:-} /usr/lib/kernel6/bin/hub_util /data/harmony/bin/hub_util; do
        [ -x $u ] || continue
        kill "$(cat /var/run/usbhid.pid 2>/dev/null)" 2>/dev/null
        $u usbhid > /tmp/usbhid.log 2>&1 &
        echo $! > /var/run/usbhid.pid
        break
    done
    echo device > /tmp/harmony_usb_port_mode
    ;;
stop)
    kill "$(cat /var/run/udhcpd.gadget0.pid 2>/dev/null)" "$(cat /var/run/usbhid.pid 2>/dev/null)" 2>/dev/null
    sh "$0" down
    ;;
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
    [ -d $G ] || exit 0
    echo "" > $G/UDC 2>/dev/null
    # configfs: links, then configs, functions and strings, then the gadget
    rm -f $G/configs/c.1/hid.wifi $G/configs/c.1/ncm.net
    rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/hid.wifi $G/functions/ncm.net \
        $G/strings/0x409 $G 2>/dev/null
    ;;
*)
    echo "usage: $0 start|stop|up|down" >&2
    exit 2
    ;;
esac
