#!/bin/sh
# Codex BT Reconnect: fast reconnect tries for 30 seconds after activity start
PID_FILE="/tmp/codex_bt_reconnect.pid"
LOG="/tmp/codex-bt-reconnect.log"
STATUS_FILE="/tmp/bthid_status"
CMD_FILE="/tmp/codex_btstack_cmd"
TARGET_FILE="/data/codex/bthid_target"
HOST_TARGET_FILE="/data/codex/bthid_host_target"

# Log rotation (tail not available on hub busybox)
if [ -f "$LOG" ] && [ "$(wc -l < "$LOG" 2>/dev/null || echo 0)" -gt 300 ]; then
    sed -e :a -e '$q;N;101,$D;ba' "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
fi

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $1" >> "$LOG"
}

# Single instance check: kill previous instance to restart retry window
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
        kill -9 "$OLD_PID" 2>/dev/null
    fi
fi
echo $$ > "$PID_FILE"
trap 'rm -f "$PID_FILE"' EXIT INT TERM

TARGET="$1"
DURATION="${2:-30}"
DELAY="${3:-0}"
if [ "$DELAY" -gt 0 ]; then
    log "BT reconnect: waiting ${DELAY}s before starting supervision"
    sleep "$DELAY"
fi
if [ "$DURATION" -lt 3 ]; then
    DURATION=3
fi
MAX_ATTEMPTS=$(( DURATION / 3 ))
if [ "$MAX_ATTEMPTS" -lt 1 ]; then
    MAX_ATTEMPTS=1
fi

if [ -z "$TARGET" ]; then
    if [ -f "$HOST_TARGET_FILE" ]; then
        TARGET=$(sed -n 's/.*bdaddr=//p' "$HOST_TARGET_FILE" 2>/dev/null | sed -e 's/[[:space:]]//g' | awk '{print; exit}')
    fi
    if [ -z "$TARGET" ] && [ -f "$TARGET_FILE" ]; then
        TARGET=$(sed -n 's/.*bdaddr=//p' "$TARGET_FILE" 2>/dev/null | sed -e 's/[[:space:]]//g')
    fi
    if [ -z "$TARGET" ] && [ -f "$HOST_TARGET_FILE" ]; then
        TARGET=$(awk 'match($0, /([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/) {print substr($0, RSTART, RLENGTH); exit}' "$HOST_TARGET_FILE" 2>/dev/null)
    fi
fi

if [ -z "$TARGET" ]; then
    log "BT reconnect: no target Bluetooth MAC found"
    exit 0
fi

TARGET=$(echo "$TARGET" | tr '[:lower:]' '[:upper:]')

is_connected() {
    [ -f "$STATUS_FILE" ] && grep -q '"state":"listening"' "$STATUS_FILE" 2>/dev/null
}

if is_connected; then
    log "BT reconnect: $TARGET already connected"
    exit 0
fi

log "BT reconnect supervision started for $TARGET (window=${DURATION}s, attempts=$MAX_ATTEMPTS)"

# Trigger BTstack connection once
if pidof codex_btstack >/dev/null 2>&1; then
    echo "connect_host $TARGET $DURATION" > "$CMD_FILE"
fi

ATTEMPT=1

while [ "$ATTEMPT" -le "$MAX_ATTEMPTS" ]; do
    sleep 3

    if is_connected; then
        log "BT reconnect: $TARGET connected successfully on attempt $ATTEMPT"
        exit 0
    fi

    ATTEMPT=$((ATTEMPT + 1))
done

log "BT reconnect: 90s timeout reached for $TARGET"
exit 1
