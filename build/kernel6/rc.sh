#!/bin/sh
# /cache/kernel6/rc.sh: boot script of the Linux 6.18 kernel, started by its init (init.c) as
# PID 1. It lives on /cache so it can be changed from the stock system without writing the
# kernel.
#
# It plays the part of the stock /etc/init.d/rcS: it builds the environment the HarmonyNG
# scripts were written for on the stock kernel (paths, tool names, the ath0 interface, ttyS0),
# loads the drivers, and then sources /etc/init.d/rcS.local as rcS did. rcS.local starts
# wpa_supplicant, Bluetooth and init.sh (network_manager.sh, hub_daemon, hub_rf, hub_btstack,
# SSH). Without HarmonyNG installed it brings up Wi-Fi, USB network and SSH itself.
#
# /cache/kernel6/modules/<release>  kernel modules          /mnt/data/kernel6/bin  tools
# /mnt/data                         mtd:data, the writable layer of the stock union root
# /cache/kernel6/serial-console     present: keep the serial console, no Bluetooth (shared UART)

B=/cache/bin/busybox
K=/cache/kernel6
W=/mnt/data/kernel6/bin
# The RAM root has no /bin: the applets go there, so /bin/sh (the login shell in /etc/passwd)
# and the default PATH work. (mkdir is not an ash builtin: use $B.)
$B mkdir -p /bin /sbin /usr/bin /usr/sbin /usr/share/udhcpc /var/run /var/log /var/volatile/tmp /lib
$B --install -s /bin
export PATH=/data/harmony/bin:/cache/bin:/usr/sbin:/usr/bin:/sbin:/bin
exec > /dev/console 2>&1
echo "rc.sh: $(uname -r)"
SERIAL=0
[ -f $K/serial-console ] && SERIAL=1

# --- the stock layout ---
# The stock union root put the writable layer under /: its /data, /etc, /home (root's home is
# /home/root), /root and /usr/local are the paths every script and tool uses.
for d in data etc home root; do
    [ -e /$d ] || ln -s /mnt/data/$d /$d
done
[ -e /lib/modules ] || ln -s $K/modules /lib/modules
[ -e /lib/firmware ] || ln -s /mnt/data/kernel6/firmware /lib/firmware
ln -sf ttyATH0 /dev/ttyS0      # the stock name of the UART (Bluetooth)
mount -t devpts devpts /dev/pts 2>/dev/null || { mkdir -p /dev/pts && mount -t devpts devpts /dev/pts; }
hostname -F /etc/hostname 2>/dev/null || hostname HarmonyHub
ifconfig lo 127.0.0.1 up

# modprobe that follows modules.dep (BusyBox's small one loads by trial), also for the kernel
cp $K/modprobe.sh /sbin/modprobe && chmod 755 /sbin/modprobe
echo /sbin/modprobe > /proc/sys/kernel/modprobe

# stock tool names: wpa_supplicant with the nl80211 driver (stock: athr), wlanconfig not needed
# (ath0 is the renamed wlan0), the BlueZ tools, udhcpc's default script
printf '#!/bin/sh\nexec %s -D nl80211 "$@"\n' "$W/wpa_supplicant" > /usr/sbin/wpa_supplicant
printf '#!/bin/sh\nexit 0\n' > /sbin/wlanconfig
chmod 755 /usr/sbin/wpa_supplicant /sbin/wlanconfig
ln -sf $W/wpa_cli /usr/sbin/wpa_cli
ln -sf $K/udhcpc.sh /usr/share/udhcpc/default.script
if [ $SERIAL = 0 ]; then
    for t in bccmd hciattach hciconfig; do ln -sf $W/$t /usr/sbin/$t; done
fi

# --- drivers ---
# USB: device mode by default. The OTG ID pin is not wired and reads "host"; start_usb_eth.sh
# switches to host mode for USB Ethernet and sticks.
modprobe phy-ath79-usb
modprobe ci_hdrc_usb2
R=/sys/bus/platform/devices/1b000000.usb/ci_hdrc.0/role
for i in 1 2 3 4 5; do [ -e $R ] && break; sleep 1; done
echo gadget > $R 2>/dev/null || echo "rc.sh: cannot set the USB role"

# Wi-Fi: ath9k; its interface gets the stock name ath0
if modprobe ath9k; then
    for i in 1 2 3 4 5; do [ -e /sys/class/net/wlan0 ] && break; sleep 1; done
    ip link set wlan0 name ath0 || echo "rc.sh: cannot rename wlan0"
else
    echo "rc.sh: no Wi-Fi driver"
fi

# Hub hardware with the stock interfaces (build/kernel6/drivers): Pair button and light
# (input0), RF (/dev/rfspi), IR (/dev/i2s). harmony_pimento first: it disables JTAG, whose pins
# are the RF ready line and chip select and the Pair button.
for m in harmony_pimento harmony_cc2544 harmony_i2s; do
    modprobe $m || echo "rc.sh: $m did not load"
done

# Hub Pro 2400 (skin 115) Ethernet: the AR9331 MACs and the internal switch (DSA). The stock
# kernel showed the jack as eth1; here eth1 is a bridge of the switch ports and eth0, so it has
# a link whichever port the jack is on, and network_manager.sh keeps using eth1.
skin=$(cat /data/harmony/skin 2>/dev/null)
[ -n "$skin" ] || skin=$(hub_util mfg skin 2>/dev/null)
if [ "$skin" = 115 ] && modprobe ag71xx && modprobe ar9331 && modprobe bridge; then
    for i in 1 2 3 4 5; do [ -e /sys/class/net/lan0 ] && break; sleep 1; done
    ip link set eth1 name ethsw && ip link set ethsw up
    brctl addbr eth1
    for p in eth0 lan0 lan1 lan2 lan3; do
        [ -e /sys/class/net/$p ] && brctl addif eth1 $p && ip link set $p up
    done
fi

# Bluetooth: the CSR8811 is on the only UART, also the serial console. rcS.local/init.sh load
# its PSKEYs and attach it (bccmd, hciattach on ttyS0); keep the kernel off the UART.
if [ $SERIAL = 0 ]; then
    modprobe hci_uart
    echo "rc.sh: Bluetooth takes the UART; console output from here on in /tmp/rc.log"
    exec > /tmp/rc.log 2>&1
    dmesg -n 1
fi

# --- the system ---
syslogd -C256
klogd
echo 1 > /proc/sys/vm/overcommit_memory

# USB network to a PC (RNDIS + CDC ECM): the Hub is 192.168.7.1 and gives the PC 192.168.7.2.
# (The stock system ran its usbhid on gadgetfs here; start_usb_eth.sh removes the gadget for
# host mode.)
# Its interface is renamed usb0 -> rndis0: network_manager.sh takes usb* for a USB Ethernet
# adapter.
if modprobe usb_f_ecm && modprobe usb_f_ecm_subset && modprobe g_ether &&
    ip link set usb0 name rndis0 && ifconfig rndis0 192.168.7.1 netmask 255.255.255.0 up; then
    printf 'start 192.168.7.2\nend 192.168.7.2\ninterface rndis0\nlease_file /tmp/udhcpd.leases\npidfile /var/run/udhcpd.rndis0.pid\noption subnet 255.255.255.0\n' > /tmp/udhcpd.conf
    touch /tmp/udhcpd.leases
    udhcpd /tmp/udhcpd.conf
else
    echo "rc.sh: no USB network"
fi

if [ -f /etc/init.d/rcS.local ]; then
    # As the stock rcS: source rcS.local (in a subshell: its emergency branch ends in exit, and
    # PID 1 must not exit), then start /usr/sbin/dropbear, which rcS.local set up.
    load_modules() { :; }   # the stock rcS function: the drivers are loaded above
    ( . /etc/init.d/rcS.local )
    [ -x /usr/sbin/dropbear ] && /usr/sbin/dropbear
else
    # no HarmonyNG: Wi-Fi with the stock configuration and SSH
    mkdir -p /var/run/wpa_supplicant
    /usr/sbin/wpa_supplicant -B -i ath0 -c /etc/wpa_supplicant.conf
    udhcpc -b -i ath0 -x hostname:HarmonyHub
    [ -x /data/harmony/bin/dropbearmulti ] && /data/harmony/bin/dropbearmulti dropbear -s -g -K 300
fi

echo "rc.sh: done"
if [ $SERIAL = 1 ]; then
    # Only plain sh: the BusyBox on a Hub's /cache may lack setsid and cttyhack. The sleep keeps
    # any failure from spinning.
    while true; do
        $B sh < /dev/console > /dev/console 2>&1
        sleep 1
    done
fi
# PID 1 must not exit
while true; do
    sleep 3600
done
