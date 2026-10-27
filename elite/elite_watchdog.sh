#!/bin/sh
# Harmony Elite Supervisor Watchdog
# Ensures elite_ui is kept running, turns off hardware LEDs on crash,
# and logs unexpected restarts.

LOG_FILE="/data/harmony_crash.log"
CRASH_WINDOW=60
MAX_CRASHES=5
CRASH_TIMESTAMPS=""

turn_off_lights() {
    echo 0 > /sys/class/backlight/ap3156_backlight/brightness 2>/dev/null || true
    echo 0 > /sys/class/leds/tps6507x:white/brightness 2>/dev/null || true
}

trap 'killall -9 elite_ui 2>/dev/null; turn_off_lights; exit 0' INT TERM

while true; do
    PROG=""
    if [ -x /data/elite_ui ]; then
        PROG="/data/elite_ui"
    else
        sleep 5
        continue
    fi

    # Launch binary in background and wait for it
    $PROG > /tmp/elite_ui.log 2>&1 &
    PID=$!

    wait $PID
    EXIT_CODE=$?

    # Immediately shut off backlight and button LEDs on crash/death
    turn_off_lights

    # If exit code was 0 (clean user shutdown/quit), exit watchdog loop
    if [ "$EXIT_CODE" -eq 0 ]; then
        echo "$(date) $PROG exited cleanly (code 0)" >> /tmp/elite_watchdog.log 2>/dev/null
        break
    fi

    # Exit code 137 (SIGKILL) or 143 (SIGTERM) is an intentional reload or operator kill;
    # log to RAM (/tmp) to avoid unnecessary flash wear and sync
    if [ "$EXIT_CODE" -eq 137 ] || [ "$EXIT_CODE" -eq 143 ]; then
        echo "$(date) [WATCHDOG] $PROG stopped via signal $EXIT_CODE (PID $PID). Restarting..." >> /tmp/elite_watchdog.log 2>/dev/null
        sleep 1
        continue
    fi

    NOW=$(date +%s 2>/dev/null || echo 0)
    echo "$(date) [WATCHDOG] $PROG died with exit code $EXIT_CODE (PID $PID). Restarting..." >> "$LOG_FILE" 2>/dev/null
    /data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true

    # Throttle rapid crash looping: count crashes in last 60 seconds
    NEW_TIMESTAMPS="$NOW"
    COUNT=1
    for TS in $CRASH_TIMESTAMPS; do
        DIFF=$(expr "$NOW" - "$TS" 2>/dev/null || echo 999)
        if [ "$DIFF" -lt "$CRASH_WINDOW" ]; then
            NEW_TIMESTAMPS="$NEW_TIMESTAMPS $TS"
            COUNT=$(expr "$COUNT" + 1 2>/dev/null || echo 1)
        fi
    done
    CRASH_TIMESTAMPS="$NEW_TIMESTAMPS"

    if [ "$COUNT" -ge "$MAX_CRASHES" ]; then
        echo "$(date) [WATCHDOG] Rapid crash detected ($COUNT crashes in ${CRASH_WINDOW}s). Pausing 30s before retry..." >> "$LOG_FILE" 2>/dev/null
        turn_off_lights
        sleep 30
    else
        sleep 2
    fi
done
