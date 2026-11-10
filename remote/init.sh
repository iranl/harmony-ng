#!/bin/sh
# Harmony remote service supervisor (/data/init.sh)
# Follows the same pattern as Harmony Hub /data/harmony/init.sh with start/stop/restart/status,
# plus "migrate" (also run at every start, and by remote_ui itself before it reads its config).
PATH=/data:/bin:/sbin:/usr/bin:/usr/sbin
LOG=/tmp/remote-init.log

sync_fs() {
    /data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true
}

# Builds before the elite -> remote rename used /data/elite_* names. Idempotent; a regular file
# /data/elite_ui is a binary an older installer has just put there (it looks for that name) and
# replaces /data/remote_ui. The old names stay as symlinks for supervisors and installers of
# older builds that may still be running during the transition.
migrate_legacy_names() {
    _moved=0
    if [ -f /data/elite_ui ] && [ ! -L /data/elite_ui ]; then
        mv -f /data/elite_ui /data/remote_ui && chmod 755 /data/remote_ui && _moved=1
    fi
    if [ -f /data/elite_sync ] && [ ! -L /data/elite_sync ]; then
        mv -f /data/elite_sync /data/remote_sync && chmod 755 /data/remote_sync && _moved=1
    fi
    if [ -f /data/elite_watchdog.sh ] && [ ! -L /data/elite_watchdog.sh ] && [ ! -f /data/remote_watchdog.sh ]; then
        mv -f /data/elite_watchdog.sh /data/remote_watchdog.sh && _moved=1
    fi
    for _p in elite_settings.conf:remote_settings.conf elite_ui.conf:remote_ui.conf \
              elite_rf_config.json:remote_rf_config.json elite_activity_usage.conf:remote_activity_usage.conf \
              elite_ui.bak:remote_ui.bak; do
        _o="/data/${_p%%:*}"; _n="/data/${_p##*:}"
        if [ -f "$_o" ] && [ ! -L "$_o" ] && [ ! -f "$_n" ]; then mv -f "$_o" "$_n" && _moved=1; fi
        # the old name points at the new file: a rollback to an older build keeps its settings
        [ -f "$_n" ] && [ ! -e "$_o" ] && ln -s "${_p##*:}" "$_o" 2>/dev/null
    done
    if [ -d /data/elite_conf_p ] && [ ! -d /data/remote_conf_p ]; then
        mv -f /data/elite_conf_p /data/remote_conf_p && _moved=1
    fi
    [ -e /data/remote_ui ] && [ ! -e /data/elite_ui ] && ln -s remote_ui /data/elite_ui 2>/dev/null
    [ -e /data/remote_sync ] && [ ! -e /data/elite_sync ] && ln -s remote_sync /data/elite_sync 2>/dev/null
    [ -e /data/remote_watchdog.sh ] && [ ! -e /data/elite_watchdog.sh ] && ln -s remote_watchdog.sh /data/elite_watchdog.sh 2>/dev/null
    if [ "$_moved" = "1" ]; then
        echo "$(date) Migrated /data/elite_* to /data/remote_*" >> "$LOG"
        sync_fs
    fi
    return 0
}

turn_off_lights() {
    echo 0 > /sys/class/backlight/ap3156_backlight/brightness 2>/dev/null || true
    echo 0 > /sys/class/leds/tps6507x:white/brightness 2>/dev/null || true
}

case "$1" in
    migrate)
        migrate_legacy_names
        exit 0
        ;;
    stop)
        echo "$(date) Stopping remote services..." >> "$LOG"
        # elite_*: a supervisor of a build from before the rename may still run
        touch /tmp/remote_stop /tmp/elite_stop
        killall -15 remote_ui elite_ui 2>/dev/null || true
        sleep 1
        killall -9 remote_ui elite_ui 2>/dev/null || true
        for _pf in /tmp/remote_supervisors.pid /tmp/elite_supervisors.pid; do
            if [ -f "$_pf" ]; then
                for spid in $(cat "$_pf" 2>/dev/null); do
                    [ -n "$spid" ] && kill -9 "$spid" 2>/dev/null || true
                done
                rm -f "$_pf" 2>/dev/null || true
            fi
        done
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
        if pidof remote_ui elite_ui >/dev/null 2>&1; then
            echo "remote_ui is RUNNING (PID $(pidof remote_ui elite_ui))"
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

migrate_legacy_names
rm -f /tmp/remote_stop /tmp/elite_stop 2>/dev/null || true

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
        echo "$(date) remote supervisor already running" >> "$LOG"
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
            if [ -f /data/debug_logging.conf ] || grep -q 'debug_logging=1' /data/remote_settings.conf 2>/dev/null; then
                echo "$(date) remote_ui died with exit code $EXIT_CODE. Restarting..." >> /data/harmony_crash.log
                dmesg > /data/last_kmsg.log 2>/dev/null || true
                turn_off_lights
                sync_fs
            else
                echo "$(date) remote_ui died with exit code $EXIT_CODE. Restarting..." >> /tmp/harmony_crash.log
                dmesg > /tmp/last_kmsg.log 2>/dev/null || true
                turn_off_lights
            fi
            sleep 1
        else
            sleep 2
        fi
    done
) &
echo "$!" >> /tmp/remote_supervisors.pid
