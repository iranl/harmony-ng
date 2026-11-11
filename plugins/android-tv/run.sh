#!/bin/sh
PID_FILE="/tmp/plugins/android-tv.pid"
SOCK_FILE="${HARMONY_SOCKET:-/tmp/plugins/android-tv.sock}"

case "$1" in
  start)
    [ -f "$PID_FILE" ] && kill -9 "$(cat "$PID_FILE")" 2>/dev/null
    rm -f "$SOCK_FILE" "$PID_FILE"
    mkdir -p /tmp/plugins
    ./bin/android_tv "$SOCK_FILE" > /tmp/plugins/android-tv.log 2>&1 &
    echo $! > "$PID_FILE"
    ;;
  stop)
    if [ -f "$PID_FILE" ]; then
      kill -15 "$(cat "$PID_FILE")" 2>/dev/null
      sleep 1
      kill -9 "$(cat "$PID_FILE")" 2>/dev/null
      rm -f "$PID_FILE" "$SOCK_FILE"
    fi
    ;;
  *)
    echo "Usage: $0 {start|stop}"
    exit 1
    ;;
esac
