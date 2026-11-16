#!/bin/sh
# tools/remote_render: --select past the activity list must not read outside g_activities,
# and an unreadable input file is an error message, not a crash.
# The host renderer is built with UndefinedBehaviorSanitizer (out-of-bounds index = abort).
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

CC="cc -fsanitize=undefined -fno-sanitize-recover=undefined" \
    bash "$REPO_ROOT/tools/remote_render/build.sh" "$WORK/remote_render" >/dev/null
mkdir -p "$WORK/root/data"
cat > "$WORK/root/data/remote_rf_config.json" <<'JSON'
{"version": 1, "slot": 1,
 "activities": [{"id": "1", "name": "Watch TV", "icon": "watchtv"}, {"id": "2", "name": "Music", "icon": "music"}],
 "devices": []}
JSON

for sel in 40 -3 1; do
    if ! "$WORK/remote_render" render --root "$WORK/root" --layout carousel --view activities --select "$sel" \
            -o "$WORK/out.png" >"$WORK/log" 2>&1; then
        echo "FAIL: --select $sel"; cat "$WORK/log"; exit 1
    fi
    [ -s "$WORK/out.png" ] || { echo "FAIL: no picture for --select $sel"; exit 1; }
done

# A directory is not a readable SVG: an error message and exit code 1, no crash
set +e
"$WORK/remote_render" rasterize --svg "$WORK/root" --size 40 -o "$WORK/icon.png" >"$WORK/log" 2>&1
rc=$?
set -e
[ "$rc" = 1 ] && grep -q 'cannot read' "$WORK/log" || { echo "FAIL: unreadable SVG (exit $rc)"; cat "$WORK/log"; exit 1; }

echo "PASS: remote_render keeps --select inside the activity list"
