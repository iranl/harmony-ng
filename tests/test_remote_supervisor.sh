#!/bin/sh
# init.sh supervisor loop: a remote_ui that fails right after every start is restarted with a
# growing delay (1 s three times, then 5, 30, 300 s), not once a second for ever; a remote_ui
# that ran for a while is restarted after 1 s as before. After 3 such failures, or when
# /data/remote_ui is missing, the previous version /data/remote_ui.bak is started instead
# (remote_watchdog.sh as well). Runs the loop of remote/init.sh with
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
# a loop that never ends is stopped after 50 rounds (the checks then fail)
sleep() { echo "\$1" >> "$ROOT/sleeps"; [ \$(wc -l < "$ROOT/sleeps") -lt 50 ] || exit 3; }
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

# Fallback to the previous version. ui and bak record each start (and the name they run under),
# take <seconds> of uptime and exit with <code>; the stop file is set after <starts> in total.
# fallback <ui seconds> <ui code> <bak seconds> <bak code> <starts> [no-ui]
mkprog() {
    cat > "$1" <<EOF
#!/bin/sh
echo "$2:\${0##*/}" >> "$ROOT/starts"
n=\$(wc -l < "$ROOT/starts")
read u r < "$ROOT/uptime"; echo "\$((\${u%%.*} + $3)).00 0.00" > "$ROOT/uptime"
[ \$n -ge $5 ] && touch "$ROOT/tmp/remote_stop"
exit $4
EOF
    chmod 755 "$1"
}
reset_root() {
    rm -rf "${ROOT:?}/data" "${ROOT:?}/tmp" "${ROOT:?}/sleeps" "${ROOT:?}/starts"
    mkdir -p "$ROOT/data" "$ROOT/tmp"
    echo "1000.00 900.00" > "$ROOT/uptime"
}
fallback() {
    reset_root
    [ "${6:-}" = no-ui ] || mkprog "$ROOT/data/remote_ui" ui "$1" "$2" "$5"
    mkprog "$ROOT/data/remote_ui.bak" bak "$3" "$4" "$5"
    sh "$ROOT/run.sh"
}
starts() { tr "\n" " " < "$ROOT/starts" 2>/dev/null; }

# 3. the new binary fails at once, the previous one runs: started after the third failure, 1 s
#    later as before (the hardware watchdog is not left unfed longer), under the name remote_ui
fallback 0 1 120 1 5
check '[ "$(starts)" = "ui:remote_ui ui:remote_ui ui:remote_ui bak:remote_ui ui:remote_ui " ]' "3 failures, then remote_ui.bak, after its long run remote_ui again (got: $(starts))"
check '[ "$(tr "\n" " " < $ROOT/sleeps)" = "1 1 1 1 " ]' "no longer delay before the fallback (got: $(tr '\n' ' ' < $ROOT/sleeps 2>/dev/null))"
check 'grep -q "failed 3 times in a row right after start: starting the previous version" $ROOT/tmp/remote-init.log' "the fallback is logged"

# 4. both fail at once: the fallback stays, the back-off goes on as without it
fallback 0 1 0 1 7
check '[ "$(starts)" = "ui:remote_ui ui:remote_ui ui:remote_ui bak:remote_ui bak:remote_ui bak:remote_ui bak:remote_ui " ]' "both failing: remote_ui.bak kept (got: $(starts))"
check '[ "$(tr "\n" " " < $ROOT/sleeps)" = "1 1 1 5 30 300 " ]' "back-off unchanged (got: $(tr '\n' ' ' < $ROOT/sleeps 2>/dev/null))"

# 5. /data/remote_ui is missing: remote_ui.bak is started at once
fallback 0 1 120 1 2 no-ui
check '[ "$(starts)" = "bak:remote_ui bak:remote_ui " ]' "missing remote_ui: remote_ui.bak started (got: $(starts))"

# 6. remote_watchdog.sh (used when init.sh is missing) falls back the same way
sed -e "s#/data/#@DATA@/#g" -e "s#/tmp/#@TMP@/#g" -e "s#/sys/#@TMP@/sys/#g" "$SCRIPT_DIR/../remote/remote_watchdog.sh" |
    sed -e "s#@DATA@#$ROOT/data#g" -e "s#@TMP@#$ROOT/tmp#g" > "$ROOT/wd.sh"
cat > "$ROOT/wdrun.sh" <<EOF
sleep() { echo "\$1" >> "$ROOT/sleeps"; [ \$(wc -l < "$ROOT/sleeps") -lt 50 ] || exit 3; }
dmesg() { :; }
# the crash clock moves 2 s per crash, as the real sleep 2 does
date() { if [ "\$1" = +%s ]; then t=\$(cat "$ROOT/clock" 2>/dev/null || echo 1000); echo \$((t + 2)) > "$ROOT/clock"; echo \$t; else command date; fi; }
. "$ROOT/wd.sh"
EOF
reset_root
mkdir -p "$ROOT/tmp/sys/class/backlight/ap3156_backlight" "$ROOT/tmp/sys/class/leds/tps6507x:white"
mkprog "$ROOT/data/remote_ui" ui 0 1 99
mkprog "$ROOT/data/remote_ui.bak" bak 0 0 99
sh "$ROOT/wdrun.sh"
check '[ "$(starts)" = "ui:remote_ui ui:remote_ui ui:remote_ui bak:remote_ui " ]' "remote_watchdog.sh: 3 crashes, then remote_ui.bak (got: $(starts))"
check 'grep -q "starting the previous version" $ROOT/tmp/remote_watchdog.log' "remote_watchdog.sh logs the fallback"

if [ "$FAIL" -ne 0 ]; then echo "[-] $FAIL supervisor checks failed"; exit 1; fi
echo "[+] All init.sh supervisor checks passed"
