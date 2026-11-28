#!/bin/sh
# Data and cache on a USB stick (Linux 6.18 Hub system): an ext4 stick, formatted by the Hub with
# the user's consent, holds the overlay's writable layer (data/upper, data/work) and /cache
# (cache/). The flash keeps the boot state: /cache/kernel6 (flags, a staged root file system,
# the rescue scripts) stays on flash, bound over the stick's cache/kernel6, and the flash data
# partition keeps the data as they were at the set-up.
#
#   usb_storage.sh status               mode=flash|usb|usb-missing, the stick and its space
#   usb_storage.sh devices              the USB disks: device size_kb (host mode needed)
#   usb_storage.sh scan                 USB host mode (the USB device side goes), then devices
#   usb_storage.sh setup <dev> --yes    erase <dev>, copy data and cache to it, reboot onto it
#   usb_storage.sh disable              back to the flash at the next boot (its copy from the
#                                       set-up: what changed on the stick since is not on it)
#   usb_storage.sh boot <uuid>          init (build/kernel6/init.c) only, chrooted in the root
#                                       file system: USB host mode, drivers, the stick on /mnt
#
# Without the stick (or with a stick that does not mount) init boots the rescue: the flash data,
# no HarmonyNG, SSH over USB and Wi-Fi (rc.sh, flag usb-missing). The stick must stay in while
# the Hub runs: its data are the Hub's.
FLAG=/cache/kernel6/usb-storage        # the stick's file system UUID (flash)
MISSING=/cache/kernel6/usb-missing     # init: the stick did not come at the last boot
ROLE=/sys/bus/platform/devices/1b000000.usb/ci_hdrc.0/role

die() { echo "usb_storage: $*" >&2; exit 1; }

host_mode() {
    modprobe phy-ath79-usb
    modprobe ci_hdrc_usb2
    i=0
    while [ ! -e $ROLE ] && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
    [ "$(cat $ROLE 2>/dev/null)" = host ] || echo host > $ROLE || return 1
    modprobe usb-storage
    modprobe sd_mod
    modprobe ext4
}

case "$1" in
boot)
    [ -n "$2" ] || die "no UUID"
    host_mode || die "no USB host mode"
    i=0
    until dev=$(findfs "UUID=$2" 2>/dev/null); do
        [ $i -ge 20 ] && die "no stick with file system $2"
        sleep 1
        i=$((i + 1))
    done
    mount -t ext4 -o noatime "$dev" /mnt || die "cannot mount $dev"
    if [ ! -d /mnt/data/upper ] || [ ! -d /mnt/data/work ] || [ ! -d /mnt/cache ]; then
        umount /mnt
        die "$dev holds no Hub data"
    fi
    ;;

scan)
    echo host > /tmp/harmony_usb_port_mode   # init.sh: no USB device side from now on
    kill "$(cat /var/run/udhcpd.gadget0.pid 2>/dev/null)" "$(cat /var/run/usbhid.pid 2>/dev/null)" 2>/dev/null
    sh /usr/lib/kernel6/usb_gadget.sh down 2>/dev/null
    host_mode || die "no USB host mode"
    sleep 5   # the stick's disk shows up a few seconds after host mode
    sh "$0" devices
    ;;

devices)
    for d in /sys/block/sd*; do
        [ -e "$d" ] || continue
        echo "/dev/${d##*/} $(( $(cat "$d/size") / 2 ))"
    done
    ;;

status)
    if [ -s $FLAG ] && grep -q ' /usb ' /proc/mounts; then
        echo mode=usb
        echo "uuid=$(cat $FLAG)"
        df -k /usb | awk 'NR == 2 { print "device=" $1; print "total_kb=" $2; print "free_kb=" $4 }'
    elif [ -s $FLAG ]; then
        echo mode=usb-missing
        echo "uuid=$(cat $FLAG)"
    else
        echo mode=flash
    fi
    ;;

setup)
    dev="$2"
    [ "$3" = --yes ] || die "setup erases $dev: run it with --yes"
    case "$dev" in /dev/sd[a-z] | /dev/sd[a-z][0-9]) ;; *) die "$dev is not a USB disk" ;; esac
    [ -b "$dev" ] || die "$dev not found"
    grep -q '^overlay / overlay' /proc/mounts && [ -d /overlay/upper ] ||
        die "only on a root file system boot from the flash"
    [ -s $FLAG ] && die "already set up (disable first)"
    command -v mkfs.ext4 >/dev/null || die "mkfs.ext4 missing"
    # the stick may be mounted (USB Storage card): it goes
    for m in $(grep "^${dev%[0-9]}" /proc/mounts | cut -d' ' -f2); do umount "$m" || die "$m busy"; done
    used=$(du -sk /overlay/upper /cache | awk '{ s += $1 } END { print s }')
    size=$(( $(cat /sys/block/$(basename "${dev%[0-9]}")/size) / 2 ))
    [ "$size" -gt $((used * 2 + 65536)) ] || die "$dev is too small ($size KB, data and cache use $used KB)"

    echo "usb_storage: stopping the Hub services"
    touch /tmp/harmony_stop
    killall -15 hub_rf hub_btstack hub_daemon 2>/dev/null
    sleep 2
    killall -9 hub_rf hub_btstack hub_daemon 2>/dev/null

    echo "usb_storage: formatting $dev (ext4)"
    mkfs.ext4 -F -q -t ext4 -L harmonyhub -m 0 "$dev" || die "mke2fs failed"
    uuid=$(blkid "$dev" | sed -n 's/.* UUID="\([^"]*\)".*/\1/p')
    [ -n "$uuid" ] || die "no UUID on $dev"
    mkdir -p /tmp/usbsetup
    mount -t ext4 -o noatime "$dev" /tmp/usbsetup || die "cannot mount $dev"
    mkdir -p /tmp/usbsetup/data/upper /tmp/usbsetup/data/work /tmp/usbsetup/cache
    echo "usb_storage: copying data and cache"
    # overlayfs xattrs (opaque directories) are not copied by BusyBox cp; whiteouts are
    if ! cp -a /overlay/upper/. /tmp/usbsetup/data/upper/ || ! cp -a /cache/. /tmp/usbsetup/cache/; then
        umount /tmp/usbsetup
        rm -f /tmp/harmony_stop
        die "copy failed: the Hub keeps the flash (reboot to restart its services)"
    fi
    rm -rf /tmp/usbsetup/cache/kernel6   # the flash's, bound over it at boot
    mkdir /tmp/usbsetup/cache/kernel6
    sync
    umount /tmp/usbsetup || die "cannot unmount $dev"
    rm -f $MISSING
    echo "$uuid" > $FLAG.new && mv -f $FLAG.new $FLAG || die "cannot write $FLAG"
    sync
    echo "usb_storage: $dev ($uuid) holds data and cache from the next boot: rebooting"
    killall -9 hub_rf hub_btstack 2>/dev/null
    sleep 1
    echo b > /proc/sysrq-trigger
    ;;

disable)
    [ -s $FLAG ] || die "not set up"
    rm -f $FLAG $MISSING
    sync
    echo "usb_storage: the flash holds data and cache from the next boot (as at the set-up)"
    ;;

*)
    sed -n '8,15p' "$0" >&2
    exit 2
    ;;
esac
