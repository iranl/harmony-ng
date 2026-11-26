#!/bin/sh
# /cache/kernel6/rc.sh: boot script of the Linux 6.18 kernel, started by its init (init.c) as
# PID 1. It lives on /cache so it can be changed from the stock system without writing the
# kernel. Step 3b: bring the system up far enough to reach it without serial (USB network and
# SSH); the Harmony services come later.
#
# /cache/kernel6/modules/<release>  the kernel modules (modules.tar.gz of the same build)
# /mnt/data                         mtd:data, the writable layer of the stock union root

B=/cache/bin/busybox
mkdir -p /tmp/bin
$B --install -s /tmp/bin
export PATH=/tmp/bin:/data/harmony/bin
exec > /dev/console 2>&1
echo "rc.sh: $(uname -r)"

# The stock union root put the writable layer under /; its paths (/data/harmony,
# /etc/dropbear, /root/.ssh) are the ones every script and tool uses.
for d in data etc root; do
    [ -e /$d ] || ln -s /mnt/data/$d /$d
done
mkdir -p /var/run /var/log /lib
[ -e /lib/modules ] || ln -s /cache/kernel6/modules /lib/modules
hostname HarmonyHub
ifconfig lo 127.0.0.1 up

# USB: device mode, RNDIS + CDC ECM to the PC. The Hub is 192.168.7.1 and gives the PC
# 192.168.7.2 by DHCP.
# No hotplug helper loads drivers for device tree nodes: the PHY first, then the controller.
modprobe phy-ath79-usb
modprobe ci_hdrc_usb2
if modprobe g_ether && ifconfig usb0 192.168.7.1 netmask 255.255.255.0 up; then
    cat > /tmp/udhcpd.conf <<EOF
start 192.168.7.2
end 192.168.7.2
interface usb0
lease_file /tmp/udhcpd.leases
option subnet 255.255.255.0
EOF
    touch /tmp/udhcpd.leases
    udhcpd /tmp/udhcpd.conf
else
    echo "rc.sh: no USB network"
fi

# SSH as on the stock kernel: hub_sshd picks the binary, key logins only
sh /data/harmony/hub_sshd || echo "rc.sh: no SSH server"

# Wi-Fi driver only: there is no nl80211 wpa_supplicant yet
modprobe ath9k || echo "rc.sh: ath9k did not load"

echo "rc.sh: done; console shell follows"
# Only plain sh: the BusyBox on a Hub's /cache may lack setsid and cttyhack (a loop on a
# missing applet spun forever). The sleep keeps any failure from spinning.
while true; do
    sh < /dev/console > /dev/console 2>&1
    sleep 1
done
