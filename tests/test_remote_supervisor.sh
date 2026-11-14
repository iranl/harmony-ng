#!/bin/sh
# init.sh supervisor loop: a remote_ui that fails right after every start is restarted with a
# growing delay (1 s three times, then 5, 30, 300 s), not once a second for ever; a remote_ui
# that ran for a while is restarted after 1 s as before. Runs the loop of remote/init.sh with
# /data, /tmp and /proc/uptime moved into a temporary directory, sleep recorded instead of run.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INIT="${1:-$SCRIPT_DIR/../remote/init.sh}"
ROOT="$(mktemp -d /tmp/remote_supervisor_XXXXXX)"
trap 'rm -rf "$ROOT"' EXIT
FAIL=0
check() { if eval "$1"; then echo "  PASS $2"; else echo "  FAIL $2"; FAIL=$((FAIL + 1)); fi; }

# The supervisor block only ("# 2. Supervisor loop" up to its ") &"), run in the foreground
sed -n '/^# 2\. Supervisor loop/,/^) &$/p' "$INIT" | sed -e 's/^) &$/)/' |
    sed -e "s#/data/#@DATA@/#g" -e "s#/tmp/#@TMP@/#g" -e "s#/proc/uptime#@UPTIME@#g" |
    sed -e "s#@DATA@#$ROOT/data#g" -e "s#@TMP@#$ROOT/tmp#g" -e "s#@UPTIME@#$ROOT/uptime#g" > "$ROOT/loop.sh"
cat > "$ROOT/run.sh" <<EOF
LOG="$ROOT/tmp/remote-init.log"
sleep() { echo "\$1" >> "$ROOT/sleeps"; }
turn_off_lights() { :; }
sync_fs() { :; }
. "$ROOT/loop.sh"
EOF

# run <seconds each run lasts> <starts before the stop file>
run() {
    rm -rf "$ROOT/data" "$ROOT/tmp" "$ROOT/sleeps" "$ROOT/count"
    mkdir -p "$ROOT/data" "$ROOT/tmp"
    echo "1000.00 900.00" > "$ROOT/uptime"
    cat > "$ROOT/data/remote_ui" <<EOF
#!/bin/sh
n=\$(cat "$ROOT/count" 2>/dev/null || echo 0); n=\$((n + 1)); echo \$n > "$ROOT/count"
read u r < "$ROOT/uptime"; echo "\$((\${u%%.*} + $1)).00 0.00" > "$ROOT/uptime"
[ \$n -ge $2 ] && touch "$ROOT/tmp/remote_stop"
exit 1
EOF
    chmod 755 "$ROOT/data/remote_ui"
    sh "$ROOT/run.sh"
}

echo "[*] init.sh supervisor back-off tests..."
check '[ -s "$ROOT/loop.sh" ]' "supervisor loop found in init.sh"

# 1. remote_ui fails at once on every start (cut binary, crash at start-up)
run 0 8
check '[ "$(cat $ROOT/count)" = 8 ]' "remote_ui started 8 times"
check '[ "$(tr "\n" " " < $ROOT/sleeps)" = "1 1 1 5 30 300 300 " ]' "delays 1 1 1 5 30 300 300 (got: $(tr '\n' ' ' < $ROOT/sleeps 2>/dev/null))"
check 'grep -q "failed 6 times in a row right after start, next start in 300s" $ROOT/tmp/remote-init.log' "the back-off is logged"

# 2. remote_ui runs for two minutes each time before it dies: restarted after 1 s
run 120 5
check '[ "$(tr "\n" " " < $ROOT/sleeps)" = "1 1 1 1 " ]' "long runs restart after 1 s (got: $(tr '\n' ' ' < $ROOT/sleeps 2>/dev/null))"

if [ "$FAIL" -ne 0 ]; then echo "[-] $FAIL supervisor checks failed"; exit 1; fi
echo "[+] All init.sh supervisor checks passed"
