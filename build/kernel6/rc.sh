#!/bin/sh
# Boot script of the Linux 6.18 Hub system, started by its init (build/kernel6/init.c) as PID 1:
#   root file system boot  /etc/init.d/rcS of the overlay root (build/build_rootfs6.sh)
#   fallback boot          /cache/kernel6/rc.sh (the root partition holds no root file system of
#                          ours): rescue only, the dynamic tools need the root file system's libc
#
# It plays the part of the stock /etc/init.d/rcS: it gives the HarmonyNG scripts written for the
# stock kernel their environment (paths, tool names, the ath0 interface, ttyS0), loads the
# drivers, and sources /etc/init.d/rcS.local as rcS did. rcS.local starts wpa_supplicant,
# Bluetooth and init.sh (network_manager.sh, hub_daemon, hub_rf, hub_btstack, SSH). Without
# HarmonyNG installed it brings up Wi-Fi, USB network and SSH itself.
#
# /cache/kernel6/serial-console  present: keep the serial console, no Bluetooth (shared UART)
# Pair button held at boot for 5 s: factory reset (data and cache erased by init at the reboot)

if grep -q '^overlay / overlay' /proc/mounts; then
    # root file system: everything at its place in the squashfs, the stock layout in the overlay
    ROOTFS=1
    B=/bin/busybox
    K=/usr/lib/kernel6          # its scripts
    T=/usr/lib/kernel6/bin      # its tools
else
    # fallback: a RAM root; the tools on the flash partitions. The data partition holds the
    # stock layout at its top, or in upper/ once a root file system booted (init.c moved it).
    ROOTFS=0
    D=/mnt/data
    [ -d /mnt/data/upper ] && D=/mnt/data/upper
    B=/cache/bin/busybox
    K=/cache/kernel6
    T=$D/kernel6/bin
    # The RAM root has no /bin: the applets go there, so /bin/sh (the login shell in /etc/passwd)
    # and the default PATH work. (mkdir is not an ash builtin: use $B.)
    $B mkdir -p /bin /sbin /usr/bin /usr/sbin /usr/share/udhcpc /var/run /var/log /var/volatile/tmp /lib
    $B --install -s /bin
fi
# boot stage times, also in the kernel log (dmesg): the console is not read on a normal Hub
up() { echo "rc.sh: [$(cut -d' ' -f1 /proc/uptime) s] $*"; echo "rc.sh: $*" > /dev/kmsg 2>/dev/null; }
F=/cache/kernel6               # flags
# wait for a path, up to $2 tenths of a second (0.2 s steps: a driver's device shows up fast)
waitfor() { _n=0; while [ ! -e "$1" ] && [ $_n -lt $2 ]; do sleep 0.2; _n=$((_n + 2)); done; }
export PATH=/data/harmony/bin:/cache/bin:/usr/sbin:/usr/bin:/sbin:/bin
exec > /dev/console 2>&1
echo "rc.sh: $(uname -r), $([ $ROOTFS = 1 ] && echo 'root file system' || echo 'fallback') boot"
up "start"
SERIAL=0
[ -f $F/serial-console ] && SERIAL=1
# data and cache on a USB stick (usb_storage.sh): the port stays in host mode for it; with the
# stick missing at boot (init: usb-missing) the rescue, no HarmonyNG on the flash's old data
USBDATA=0
grep -q ' /usb ' /proc/mounts && USBDATA=1
RESCUE=0
[ -f $F/usb-missing ] && RESCUE=1

# --- the stock layout ---
if [ $ROOTFS = 1 ]; then
    mount -t tmpfs tmpfs /var/volatile
    mkdir -p /var/volatile/run /var/volatile/log /var/volatile/lock /var/volatile/tmp
    # HarmonyNG's scripts call /cache/bin/busybox and /data/harmony/bin/dropbearmulti, which
    # were its own static copies on the stock root: here the root file system's dynamic ones
    # (a static copy left from before is replaced by the link, freeing its flash)
    mkdir -p /cache/bin
    [ "$(readlink /cache/bin/busybox)" = /bin/busybox ] || ln -sf /bin/busybox /cache/bin/busybox
    # /cache/bin comes first in PATH: an applet link of an older BusyBox named modprobe would
    # hide /sbin/modprobe (modules.dep) and load the drivers without what they need
    for a in modprobe depmod; do [ -L /cache/bin/$a ] && rm -f /cache/bin/$a; done
    if [ -d /data/harmony/bin ] && [ "$(readlink /data/harmony/bin/dropbearmulti)" != /usr/sbin/dropbearmulti ]; then
        ln -sf /usr/sbin/dropbearmulti /data/harmony/bin/dropbearmulti
    fi
else
    # The stock union root put the writable layer under /: its /data, /etc, /home (root's home is
    # /home/root) and /root are the paths every script and tool uses.
    for d in data etc home root; do
        [ -e /$d ] || ln -s $D/$d /$d
    done
    [ -e /lib/modules ] || ln -s $K/modules /lib/modules
    [ -e /lib/firmware ] || ln -s $D/kernel6/firmware /lib/firmware
fi
ln -sf ttyATH0 /dev/ttyS0      # the stock name of the UART (Bluetooth)
mount -t devpts devpts /dev/pts 2>/dev/null || { mkdir -p /dev/pts && mount -t devpts devpts /dev/pts; }
hostname -F /etc/hostname 2>/dev/null || hostname HarmonyHub
ifconfig lo 127.0.0.1 up

# modprobe that follows modules.dep (BusyBox's small one loads by trial), also for the kernel
if [ $ROOTFS = 0 ]; then
    cp $K/modprobe.sh /sbin/modprobe && chmod 755 /sbin/modprobe
fi
echo /sbin/modprobe > /proc/sys/kernel/modprobe

# stock tool names: wpa_supplicant with the nl80211 driver (stock: athr), wlanconfig not needed
# (ath0 is the renamed wlan0), udhcpc's default script (in the root file system these exist)
if [ $ROOTFS = 0 ]; then
    printf '#!/bin/sh\nexec %s -D nl80211 "$@"\n' "$T/wpa_supplicant" > /usr/sbin/wpa_supplicant
    printf '#!/bin/sh\nexit 0\n' > /sbin/wlanconfig
    chmod 755 /usr/sbin/wpa_supplicant /sbin/wlanconfig
    ln -sf $T/wpa_cli /usr/sbin/wpa_cli
    ln -sf $K/udhcpc.sh /usr/share/udhcpc/default.script
fi
# The BlueZ tools under their stock names only without the serial console (rcS.local and
# init.sh run them on ttyS0). In the root file system the links are in the overlay: changed
# only when the mode changed.
for t in bccmd hciattach hciconfig; do
    if [ $SERIAL = 0 ]; then
        [ -e /usr/sbin/$t ] || ln -s $T/$t /usr/sbin/$t
    else
        [ -e /usr/sbin/$t ] && rm -f /usr/sbin/$t
    fi
done

up "layout done"
# --- drivers ---
# USB: device mode by default. The OTG ID pin is not wired and reads "host"; start_usb_eth.sh
# switches to host mode for USB Ethernet and sticks.
modprobe phy-ath79-usb
modprobe ci_hdrc_usb2
R=/sys/bus/platform/devices/1b000000.usb/ci_hdrc.0/role
waitfor $R 50
[ $USBDATA = 1 ] && echo host > /tmp/harmony_usb_port_mode   # init.sh: no USB device side
# The root file system's firmware first (regulatory.db for cfg80211, which loads it with the
# Wi-Fi driver; the USB Wi-Fi adapters' firmware); /lib/firmware (the data partition: PSKEY
# files) stays the fallback
[ -f /usr/lib/kernel6/firmware/regulatory.db ] &&
    echo /usr/lib/kernel6/firmware > /sys/module/firmware_class/parameters/path

# USB Wi-Fi adapter (MediaTek MT7610U, MT7612U: mt76x0u, mt76x2u; Realtek RTL8821CU/8811CU,
# RTL8821AU/8811AU, RTL8812AU, RTL8812BU/8822BU: rtw88_8821cu, rtw88_8821au, rtw88_8812au,
# rtw88_8822bu), looked for only when turned on in the web UI (USB_WIFI_ADAPTER=1 in
# ethernet.conf; off by default, then the boot does not touch the USB port for it). One plugged
# in at boot is the Hub's Wi-Fi in place of the built-in radio (ath9k not loaded), under the same
# name ath0. The port stays in host mode for the boot then: no USB device side (Wi-Fi setup and
# network over USB), USB Ethernet and sticks through a USB hub. /tmp/harmony_wifi_adapter names
# its driver (network_manager.sh keeps host mode for it). Root file system boot only: the firmware is in the squashfs, and the rescue keeps
# the built-in radio and the USB device side.
# The driver for a plugged-in adapter: the interfaces' modaliases against these drivers' aliases.
# Only the aliases of the same vendor and product id go through the glob match (theirs all name
# both): all of them took 0.8 s per pass on the Hub, these 0.3 s.
usb_wifi_driver() {
    for f in /sys/bus/usb/devices/*-[1-9]*:*/modalias; do
        [ -e "$f" ] || continue
        m=$(cat "$f")
        grep "^alias ${m%%d*}" "/lib/modules/$(uname -r)/modules.alias" |
            grep -e ' mt76x[02]u$' -e ' rtw88_88[0-9][0-9][abc]u$' | while read -r a pat d; do
                case "$m" in $pat) echo "$d"; break ;; esac
            done | grep . && return 0
    done
    return 1
}
# USB interfaces of plugged-in devices (the root hub's 1-0:1.0 not counted)
usb_interfaces() { ls -d /sys/bus/usb/devices/*-[1-9]*:* 2>/dev/null | wc -l; }
uptime_ds() { sed 's/^\([0-9]*\)\.\([0-9]\).*/\1\2/' /proc/uptime; }
WIFI_DRV=""
if [ $ROOTFS = 1 ] && [ $RESCUE = 0 ] &&
   tr -d '\r"' < /data/harmony/ethernet.conf 2>/dev/null | grep -qx 'USB_WIFI_ADAPTER=1'; then
    if [ $USBDATA = 1 ]; then
        # host mode since init mounted the stick: the devices are there
        WIFI_DRV=$(usb_wifi_driver)
    else
        echo host > $R 2>/dev/null
        # A device shows up about 0.3 s after host mode, the ones behind a USB hub within 1.5 s
        # (seen on a Hub). The search ends at the adapter; with nothing on the port after 1.6 s
        # (a computer, a charger); when no new interface came for 1 s; at 6 s at the latest.
        t0=$(uptime_ds)
        last=0
        seen=$t0
        while :; do
            WIFI_DRV=$(usb_wifi_driver) && break
            now=$(uptime_ds)
            cnt=$(usb_interfaces)
            [ "$cnt" != "$last" ] && { last=$cnt; seen=$now; }
            [ "$cnt" = 0 ] && [ $((now - t0)) -ge 16 ] && break
            [ "$cnt" != 0 ] && [ $((now - seen)) -ge 10 ] && break
            [ $((now - t0)) -ge 60 ] && break
            sleep 0.2
        done
        up "USB Wi-Fi adapter search: ${WIFI_DRV:-none} ($(( $(uptime_ds) - t0 ))00 ms, $(usb_interfaces) USB interfaces)"
    fi
    if [ -n "$WIFI_DRV" ] && modprobe "$WIFI_DRV"; then
        waitfor /sys/class/net/wlan0 50   # after its firmware loaded
    fi
    if [ -n "$WIFI_DRV" ] && [ -e /sys/class/net/wlan0 ]; then
        # the built-in radio's MAC (mfg partition 0x1002, as the DTS gives it to ath9k): the Hub
        # keeps its DHCP address and reservation with the adapter
        n=$(sed -n 's/^mtd\([0-9]*\):.*"mfg"$/\1/p' /proc/mtd)
        mac=$(od -An -tx1 -j 4098 -N6 /dev/mtdblock$n 2>/dev/null | tr -s ' \n' '::' | sed 's/^://; s/:$//')
        case "$mac" in
            ??:??:??:??:??:??) ip link set wlan0 address "$mac" || echo "rc.sh: cannot set the adapter's MAC" ;;
        esac
        echo "$WIFI_DRV" > /tmp/harmony_wifi_adapter
        echo host > /tmp/harmony_usb_port_mode
        up "USB Wi-Fi adapter ($WIFI_DRV): the Hub's Wi-Fi, USB port in host mode"
    else
        [ -n "$WIFI_DRV" ] && up "USB Wi-Fi adapter ($WIFI_DRV) did not start: built-in Wi-Fi"
        WIFI_DRV=""
    fi
fi
if [ $USBDATA = 0 ] && [ -z "$WIFI_DRV" ]; then
    echo gadget > $R 2>/dev/null || echo "rc.sh: cannot set the USB role"
fi

up "USB drivers"
# Wi-Fi: the USB adapter or ath9k; its interface gets the stock name ath0
if [ -n "$WIFI_DRV" ] || modprobe ath9k; then
    waitfor /sys/class/net/wlan0 50
    ip link set wlan0 name ath0 || echo "rc.sh: cannot rename wlan0"
else
    echo "rc.sh: no Wi-Fi driver"
fi

up "Wi-Fi driver"
# Hub hardware with the stock interfaces (build/kernel6/drivers): Pair button and light
# (input0), RF (/dev/rfspi), IR (/dev/i2s). harmony_pimento first: it disables JTAG, whose pins
# are the RF ready line and chip select and the Pair button.
for m in harmony_pimento evdev harmony_cc2544 harmony_i2s; do
    modprobe $m || echo "rc.sh: $m did not load"
done

# Factory reset: the Pair button (harmony_pimento's button attribute) held from power on until
# here and for 5 s more. The light flashes red fast, init erases the data partition at the
# reboot.
pressed() { [ "$(cat /sys/class/input/input0/button 2>/dev/null)" = 1 ]; }
if pressed; then
    i=0
    while [ $i -lt 10 ] && pressed; do sleep 0.5; i=$((i + 1)); done
    if [ $i = 10 ]; then
        echo "rc.sh: Pair button held: factory reset"
        echo 2 > /sys/class/input/input0/led
        touch $F/factory-reset
        sync
        sleep 2
        echo b > /proc/sysrq-trigger
    fi
fi

# Hub Pro 2400 (skin 115) Ethernet: the AR9331 MACs and the internal switch (DSA). The stock
# kernel showed the jack as eth1; here eth1 is a bridge of the switch ports and eth0, so it has
# a link whichever port the jack is on, and network_manager.sh keeps using eth1.
skin=$(cat /data/harmony/skin 2>/dev/null)
[ -n "$skin" ] || skin=$($T/hub_util mfg skin 2>/dev/null)
if [ "$skin" = 115 ] && modprobe ag71xx && modprobe ar9331 && modprobe bridge; then
    waitfor /sys/class/net/lan0 50
    ip link set eth1 name ethsw && ip link set ethsw up
    brctl addbr eth1
    for p in eth0 lan0 lan1 lan2 lan3; do
        [ -e /sys/class/net/$p ] && brctl addif eth1 $p && ip link set $p up
    done
    # the bridge keeps eth0's fixed MAC (DTS: Wi-Fi MAC + 1), whichever port joined first
    ip link set eth1 address "$(cat /sys/class/net/eth0/address)"
fi

up "Hub drivers, factory reset check, Pro Ethernet"
# Bluetooth: the CSR8811 is on the only UART, also the serial console. rcS.local/init.sh load
# its PSKEYs and attach it (bccmd, hciattach on ttyS0); keep the kernel off the UART.
if [ $SERIAL = 0 ]; then
    modprobe hci_uart
    echo "rc.sh: Bluetooth takes the UART; console output from here on in /tmp/rc.log"
    # stdin too: PID 1 holding /dev/console (the UART) kept the tty open after hciattach ended,
    # so its line discipline (N_HCI) stayed and every later bccmd (ps5-wake, the init.sh
    # re-attach) talked to the kernel's BCSP instead of the chip ("Initialization timed out")
    exec < /dev/null > /tmp/rc.log 2>&1
    dmesg -n 1
fi

# --- the system ---
syslogd -C256
klogd
# Hardware watchdog (ath79_wdt, at most about 21 s): the kernel restarts the Hub when nothing
# feeds it for 15 s, after a hang that stops the scheduler. A panic restarts it after 5 s
# (CONFIG_PANIC_TIMEOUT). Killed, the Hub restarts 15 s later: it runs as "busybox", because
# HarmonyNG scripts before the 6.18 system (rcS.local, init.sh) "killall -9 watchdog" (the stock
# one), which restarted the Hub over and over after an update of the system alone.
[ -c /dev/watchdog ] && /bin/busybox watchdog -T 15 -t 5 /dev/watchdog
# Crash records of the last warm reset (pstore/ramoops, DTS reserved-memory): the kernel log up
# to the reset (console-ramoops-0) and oops/panic dumps (dmesg-ramoops-N) go to flash, the five
# newest kept; then the RAM copies are cleared
mkdir -p /sys/fs/pstore
if mount -t pstore pstore /sys/fs/pstore 2>/dev/null && [ -n "$(ls /sys/fs/pstore 2>/dev/null)" ] &&
   [ -d /data/harmony ]; then
    C=/data/harmony/crashlogs
    mkdir -p $C
    n=$(($(cat $C/.next 2>/dev/null || echo 0) + 1))
    for f in /sys/fs/pstore/*; do
        cp "$f" "$C/$n-${f##*/}.txt" && rm -f "$f"
    done
    echo $n > $C/.next
    rm -f $C/$((n - 5))-*.txt
    echo "rc.sh: crash records of the last reset saved as $C/$n-*.txt"
fi
echo 1 > /proc/sys/vm/overcommit_memory
# zram swap: a quarter of the RAM (16 MB on a 64 MB Hub, 8 MB on a 32 MB one), LZ4. A safety
# margin for peaks (icons, plugin unpacks in /tmp, Bluetooth with several plugins), not room to
# run more: low swappiness, so pages go there only under memory pressure. GET /api/system-info
# shows how much is in use (swap_total_kb, swap_free_kb).
Z=/sys/block/zram0
if [ -e $Z ] || modprobe zram 2>/dev/null; then
    mem=$(sed -n 's/^MemTotal: *\([0-9]*\) kB/\1/p' /proc/meminfo)
    echo lz4 > $Z/comp_algorithm 2>/dev/null
    if echo "$((mem / 4))K" > $Z/disksize && mkswap /dev/zram0 > /dev/null && swapon -p 100 /dev/zram0; then
        echo 20 > /proc/sys/vm/swappiness
    else
        echo "rc.sh: no zram swap"
    fi
fi

up "syslog, watchdog, crash records, zram"
# USB device side (usb_gadget.sh): the HID device of the stock USB Wi-Fi setup (046D:C129,
# relayed to hub_util usbwifi by hub_util usbhid) and a network to the PC (CDC NCM): the Hub is
# 192.168.7.1 and gives the PC 192.168.7.2. The network interface is renamed usb0 -> gadget0:
# network_manager.sh takes usb* for a USB Ethernet adapter. start_usb_eth.sh takes all of it
# down for host mode.
if [ $USBDATA = 0 ] && [ -z "$WIFI_DRV" ] && HUB_UTIL=$T/hub_util sh $K/usb_gadget.sh start; then
    :
else
    echo "rc.sh: no USB device side"
fi

[ $RESCUE = 1 ] && echo "rc.sh: the USB stick with the Hub's data is missing: rescue (no HarmonyNG)"
up "USB device side"
if [ -f /etc/init.d/rcS.local ] && [ $RESCUE = 0 ]; then
    # As the stock rcS: source rcS.local (in a subshell: its emergency branch ends in exit, and
    # PID 1 must not exit), then start /usr/sbin/dropbear, which rcS.local set up.
    load_modules() { :; }   # the stock rcS function: the drivers are loaded above
    ( . /etc/init.d/rcS.local )
    # key logins only (-s -g): root has no password; a wrapper of rcS.local adds the same flags
    [ -x /usr/sbin/dropbear ] && /usr/sbin/dropbear -s -g -K 300 -R
else
    # no HarmonyNG: Wi-Fi with the stock configuration and SSH
    mkdir -p /var/run/wpa_supplicant
    /usr/sbin/wpa_supplicant -B -i ath0 -c /etc/wpa_supplicant.conf
    udhcpc -b -i ath0 -x hostname:HarmonyHub
    for d in /usr/sbin/dropbearmulti /data/harmony/bin/dropbearmulti; do
        [ -x $d ] && { $d dropbear -s -g -K 300; break; }
    done
fi

up "done (rcS.local returned)"
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
