#!/bin/sh
PID_FILE="/tmp/plugins/roku-ecp.pid"
SOCK_FILE="${HARMONY_SOCKET:-/tmp/plugins/roku-ecp.sock}"

case "$1" in
  start)
    [ -f "$PID_FILE" ] && kill -9 "$(cat "$PID_FILE")" 2>/dev/null
    rm -f "$SOCK_FILE" "$PID_FILE"
    mkdir -p /tmp/plugins
    ./bin/roku_ecp "$SOCK_FILE" > /tmp/plugins/roku-ecp.log 2>&1 &
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
  status)
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      echo "Running (PID $(cat "$PID_FILE"))"
      exit 0
    fi
    echo "Stopped"
    exit 1
    ;;
  *)
    echo "Usage: $0 {start|stop|status}"
    exit 1
    ;;
esac
