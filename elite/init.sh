#!/bin/sh
# Harmony Elite Service Supervisor (/data/init.sh)
# Follows the same pattern as Harmony Hub /data/harmony/init.sh with start/stop/restart/status
PATH=/data:/bin:/sbin:/usr/bin:/usr/sbin
LOG=/tmp/elite-init.log

sync_fs() {
    /data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true
}

turn_off_lights() {
    echo 0 > /sys/class/backlight/ap3156_backlight/brightness 2>/dev/null || true
    echo 0 > /sys/class/leds/tps6507x:white/brightness 2>/dev/null || true
}

case "$1" in
    stop)
        echo "$(date) Stopping elite services..." >> "$LOG"
        touch /tmp/elite_stop
        killall -15 elite_ui 2>/dev/null || true
        sleep 1
        killall -9 elite_ui 2>/dev/null || true
        if [ -f /tmp/elite_supervisors.pid ]; then
            for spid in $(cat /tmp/elite_supervisors.pid 2>/dev/null); do
                [ -n "$spid" ] && kill -9 "$spid" 2>/dev/null || true
            done
            rm -f /tmp/elite_supervisors.pid 2>/dev/null || true
        fi
        for p in $(ps | grep 'init\.sh' | grep -v 'grep' | sed 's/^[ ]*//' | cut -d' ' -f1 2>/dev/null); do
            [ -n "$p" ] && [ "$p" != "$$" ] && kill -9 "$p" 2>/dev/null || true
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
        if pidof elite_ui >/dev/null 2>&1; then
            echo "elite_ui is RUNNING (PID $(pidof elite_ui))"
            exit 0
        else
            echo "elite_ui is STOPPED"
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

rm -f /tmp/elite_stop 2>/dev/null || true

# Avoid duplicate supervisor spawns
if [ -f /tmp/elite_supervisors.pid ]; then
    _any_alive=0
    for spid in $(cat /tmp/elite_supervisors.pid 2>/dev/null); do
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

# 0. Early watchdog neuter and neuter stock binaries
touch /etc/nowatchdog 2>/dev/null || true
chmod -x /usr/bin/watchdog /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true
killall -9 watchdog hal luaworks luadraws lua luajit 2>/dev/null || true

# 1. Ensure USB Ethernet and Dropbear are active
chmod -x /usr/bin/usbhid 2>/dev/null || true
/usr/bin/usbeth start 2>/dev/null || true
if [ -x /data/dropbear ]; then
    /data/dropbear -R -p 0.0.0.0:22 2>/dev/null || /data/dropbear -R -B 2>/dev/null || true
elif [ -x /usr/sbin/dropbear ]; then
    /usr/sbin/dropbear -R -p 0.0.0.0:22 2>/dev/null || /usr/sbin/dropbear -R -B 2>/dev/null || true
fi

# 2. Supervisor loop for elite_ui in background
(
    echo "$$" >> /tmp/elite_supervisors.pid
    while [ ! -f /tmp/elite_stop ]; do
        if [ -x /data/elite_ui ]; then
            echo "$(date) Starting /data/elite_ui..." >> "$LOG"
            /data/elite_ui > /tmp/elite_ui.log 2>&1
            EXIT_CODE=$?
            if [ -f /tmp/elite_stop ] || [ "$EXIT_CODE" -eq 0 ]; then
                turn_off_lights
                break
            fi
            if [ "$EXIT_CODE" -eq 137 ] || [ "$EXIT_CODE" -eq 143 ]; then
                # Intentional kill / reload signal
                turn_off_lights
                sleep 1
                continue
            fi
            echo "$(date) elite_ui died with exit code $EXIT_CODE. Restarting..." >> /data/harmony_crash.log
            turn_off_lights
            sync_fs
            sleep 1
        else
            sleep 2
        fi
    done
) &
