#!/bin/sh
# Harmony Elite Service Supervisor (/data/init.sh)
# Follows the same pattern as Harmony Hub /data/harmony/init.sh with start/stop/restart/status
PATH=/data:/bin:/sbin:/usr/bin:/usr/sbin
LOG=/tmp/remote-init.log

sync_fs() {
    /data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true
}

turn_off_lights() {
    echo 0 > /sys/class/backlight/ap3156_backlight/brightness 2>/dev/null || true
    echo 0 > /sys/class/leds/tps6507x:white/brightness 2>/dev/null || true
}

case "$1" in
    stop)
        echo "$(date) Stopping elite services..." >> "$LOG"
        touch /tmp/remote_stop
        killall -15 remote_ui 2>/dev/null || true
        sleep 1
        killall -9 remote_ui 2>/dev/null || true
        if [ -f /tmp/remote_supervisors.pid ]; then
            for spid in $(cat /tmp/remote_supervisors.pid 2>/dev/null); do
                [ -n "$spid" ] && kill -9 "$spid" 2>/dev/null || true
            done
            rm -f /tmp/remote_supervisors.pid 2>/dev/null || true
        fi
        # Not the caller: a shell that runs "init.sh stop" has init.sh in its command line
        for p in $(ps | grep 'init\.sh' | grep -v 'grep' | sed 's/^[ ]*//' | cut -d' ' -f1 2>/dev/null); do
            [ -n "$p" ] && [ "$p" != "$$" ] && [ "$p" != "$PPID" ] && kill -9 "$p" 2>/dev/null || true
        done
        turn_off_lights
        sync_fs
        exit 0
        ;;
    restart)
        $0 stop
        sleep 1
        $0 start &
        exit 0
        ;;
    status)
        if pidof remote_ui >/dev/null 2>&1; then
            echo "remote_ui is RUNNING (PID $(pidof remote_ui))"
            exit 0
        else
            echo "remote_ui is STOPPED"
            exit 1
        fi
        ;;
    start)
        # Continue below
        ;;
    *)
        # Default to start
        ;;
esac

rm -f /tmp/remote_stop 2>/dev/null || true

# Avoid duplicate supervisor spawns
if [ -f /tmp/remote_supervisors.pid ]; then
    _any_alive=0
    for spid in $(cat /tmp/remote_supervisors.pid 2>/dev/null); do
        if [ -d "/proc/$spid" ]; then
            _any_alive=1
            break
        fi
    done
    if [ "$_any_alive" = "1" ]; then
        echo "$(date) elite supervisor already running" >> "$LOG"
        exit 0
    fi
fi
rm -f /tmp/remote_supervisors.pid 2>/dev/null || true

# 0. Early watchdog neuter and neuter stock binaries
touch /etc/nowatchdog 2>/dev/null || true
chmod -x /usr/bin/watchdog /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true
killall -9 watchdog hal luaworks luadraws lua luajit 2>/dev/null || true

# Kernel auto-reboot on panic, oops, or hung D-state task
echo 3 > /proc/sys/kernel/panic 2>/dev/null || true
echo 1 > /proc/sys/kernel/panic_on_oops 2>/dev/null || true
echo 30 > /proc/sys/kernel/hung_task_timeout_secs 2>/dev/null || true
echo 1 > /proc/sys/kernel/hung_task_panic 2>/dev/null || true

# 1. Ensure USB Ethernet and Dropbear are active if USB connected
chmod -x /usr/bin/usbhid 2>/dev/null || true
POWERSOURCE="$(cat /sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource 2>/dev/null)"
if [ "$POWERSOURCE" = "usb" ] || [ -z "$POWERSOURCE" ]; then
    if [ ! -d /sys/class/net/usb0 ]; then
        /usr/bin/usbeth start &
    fi
    if ! pidof dropbear >/dev/null 2>&1 && ! pidof dropbearmulti >/dev/null 2>&1; then
        if [ -x /data/dropbearmulti ]; then
            /data/dropbearmulti dropbear -R -p 0.0.0.0:22 2>/dev/null &
        elif [ -x /data/dropbear ]; then
            /data/dropbear -R -p 0.0.0.0:22 2>/dev/null &
        elif [ -x /usr/sbin/dropbear ]; then
            /usr/sbin/dropbear -R -p 0.0.0.0:22 2>/dev/null &
        fi
    fi
fi

# 2. Supervisor loop for remote_ui in background
(
    while [ ! -f /tmp/remote_stop ]; do
        if [ -x /data/remote_ui ]; then
            echo "$(date) Starting /data/remote_ui..." >> "$LOG"
            /data/remote_ui > /tmp/remote_ui.log 2>&1
            EXIT_CODE=$?
            if [ -f /tmp/remote_stop ]; then
                turn_off_lights
                break
            fi
            if [ "$EXIT_CODE" -eq 0 ]; then
                turn_off_lights
                sleep 1
                continue
            fi
            if [ "$EXIT_CODE" -eq 137 ] || [ "$EXIT_CODE" -eq 143 ]; then
                # Intentional kill / reload signal
                turn_off_lights
                sleep 1
                continue
            fi
            echo "$(date) remote_ui died with exit code $EXIT_CODE. Restarting..." >> /data/harmony_crash.log
            dmesg > /data/last_kmsg.log 2>/dev/null || true
            turn_off_lights
            sync_fs
            sleep 1
        else
            sleep 2
        fi
    done
) &
echo "$!" >> /tmp/remote_supervisors.pid
