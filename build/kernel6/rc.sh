#!/bin/sh
# /cache/kernel6/rc.sh: boot script of the Linux 6.18 kernel, started by its init (init.c) as
# PID 1. It lives on /cache so it can be changed from the stock system without writing the
# kernel. Step 3b: bring the system up far enough to reach it without serial (USB network and
# SSH); the Harmony services come later.
#
# /cache/kernel6/modules/<release>  the kernel modules (modules.tar.gz of the same build)
# /mnt/data                         mtd:data, the writable layer of the stock union root

B=/cache/bin/busybox
# The RAM root has no /bin: the applets go there, so /bin/sh (the login shell in /etc/passwd)
# and the default PATH of SSH logins work. (mkdir is not an ash builtin: use $B.)
$B mkdir -p /bin
$B --install -s /bin
export PATH=/bin:/data/harmony/bin
echo /bin/modprobe > /proc/sys/kernel/modprobe
exec > /dev/console 2>&1
echo "rc.sh: $(uname -r)"

# BusyBox modprobe on these Hubs does not find modules in this tree ("module not found" while
# insmod of the same file works): load a module and what it needs, in modules.dep order.
kmod() {
    _d=/lib/modules/$(uname -r)
    _l=$(grep "/$1.ko:" $_d/modules.dep) || { echo "kmod: no $1"; return 1; }
    for _m in $(echo ${_l%%:*} ${_l#*:} | tr ' ' '
' | awk '{a[NR]=$0} END {for (i=NR; i>0; i--) print a[i]}'); do
        _n=$(basename $_m .ko | tr - _)
        grep -q "^$_n " /proc/modules || insmod $_d/$_m || return 1
    done
}

# The stock union root put the writable layer under /; its paths (/data/harmony,
# /etc/dropbear, /home/root/.ssh: root's home in /etc/passwd) are the ones every script and
# tool uses.
for d in data etc home root; do
    [ -e /$d ] || ln -s /mnt/data/$d /$d
done
mkdir -p /var/run /var/log /lib
[ -e /lib/modules ] || ln -s /cache/kernel6/modules /lib/modules
hostname HarmonyHub
ifconfig lo 127.0.0.1 up

# USB: device mode, RNDIS + CDC ECM to the PC. The Hub is 192.168.7.1 and gives the PC
# 192.168.7.2 by DHCP.
# No hotplug helper loads drivers for device tree nodes: the PHY first, then the controller.
kmod phy-ath79-usb
kmod ci_hdrc_usb2
if kmod usb_f_ecm && kmod usb_f_ecm_subset && kmod g_ether && ifconfig usb0 192.168.7.1 netmask 255.255.255.0 up; then
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

# SSH as on the stock kernel: hub_sshd picks the binary, key logins only. Installs older than
# hub_sshd have only the multi-call binary.
if [ -f /data/harmony/hub_sshd ]; then
    sh /data/harmony/hub_sshd
elif [ -x /data/harmony/bin/dropbearmulti ]; then
    /data/harmony/bin/dropbearmulti dropbear -s -g -K 300
else
    false
fi || echo "rc.sh: no SSH server"

# Wi-Fi: ath9k, wpa_supplicant (nl80211, /mnt/data/kernel6/bin) with the stock
# /etc/wpa_supplicant.conf, DHCP (lease script /cache/kernel6/udhcpc.sh).
W=/mnt/data/kernel6/bin
if kmod ath9k && [ -x $W/wpa_supplicant ]; then
    mkdir -p /var/run/wpa_supplicant
    $W/wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf
    udhcpc -b -i wlan0 -x hostname:HarmonyHub -s /cache/kernel6/udhcpc.sh
else
    echo "rc.sh: no Wi-Fi"
fi

echo "rc.sh: done; console shell follows"
# Only plain sh: the BusyBox on a Hub's /cache may lack setsid and cttyhack (a loop on a
# missing applet spun forever). The sleep keeps any failure from spinning.
while true; do
    $B sh < /dev/console > /dev/console 2>&1
    sleep 1
done
