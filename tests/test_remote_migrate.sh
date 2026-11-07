#!/bin/sh
# init.sh migrate: /data/elite_* (builds before the elite -> remote rename) -> /data/remote_*,
# with the old names left as links. Runs a copy of remote/init.sh with /data and /tmp moved
# into a temporary directory.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(mktemp -d /tmp/remote_migrate_XXXXXX)"
trap 'rm -rf "$ROOT"' EXIT
FAIL=0
check() { if eval "$1"; then echo "  PASS $2"; else echo "  FAIL $2"; FAIL=$((FAIL + 1)); fi; }
mkinit() {
    # placeholders first: $ROOT itself starts with /tmp/
    sed -e "s#/data/#@DATA@/#g" -e "s#/tmp/#@TMP@/#g" "$SCRIPT_DIR/../remote/init.sh" |
        sed -e "s#@DATA@#$ROOT/data#g" -e "s#@TMP@#$ROOT/tmp#g" > "$ROOT/init.sh"
}
reset() { rm -rf "$ROOT/data" "$ROOT/tmp"; mkdir -p "$ROOT/data" "$ROOT/tmp"; }
D="$ROOT/data"
mkinit

echo "[*] init.sh migrate tests..."
# 1. a remote on a build from before the rename
reset
echo bin-old > "$D/elite_ui"; echo sync > "$D/elite_sync"; echo wd > "$D/elite_watchdog.sh"
echo settings > "$D/elite_settings.conf"; echo rf > "$D/elite_rf_config.json"; echo usage > "$D/elite_activity_usage.conf"
mkdir -p "$D/elite_conf_p"; echo kept > "$D/elite_conf_p/x"
sh "$ROOT/init.sh" migrate
check '[ "$(cat $D/remote_ui)" = bin-old ] && [ -x $D/remote_ui ]' "binary moved to remote_ui"
check '[ -L $D/elite_ui ] && [ "$(readlink $D/elite_ui)" = remote_ui ]' "elite_ui is a link to remote_ui"
check '[ "$(cat $D/remote_sync)" = sync ] && [ -L $D/elite_sync ]' "sync helper moved, old name a link"
check '[ "$(cat $D/remote_watchdog.sh)" = wd ] && [ -L $D/elite_watchdog.sh ]' "watchdog moved, old name a link"
check '[ "$(cat $D/remote_settings.conf)" = settings ] && [ "$(cat $D/elite_settings.conf)" = settings ] && [ -L $D/elite_settings.conf ]' "settings moved, old name a link (rollback keeps them)"
check '[ "$(cat $D/remote_rf_config.json)" = rf ] && [ "$(cat $D/remote_activity_usage.conf)" = usage ]' "RF config and usage moved"
check '[ "$(cat $D/remote_conf_p/x)" = kept ] && [ ! -e $D/elite_conf_p ]' "factory-reset backup directory moved"
check 'grep -q "Migrated" $ROOT/tmp/remote-init.log' "the migration is logged"

# 2. a second run changes nothing
ls -l "$D" > "$ROOT/before"; sh "$ROOT/init.sh" migrate; ls -l "$D" > "$ROOT/after"
check 'cmp -s $ROOT/before $ROOT/after' "idempotent"

# 3. an older installer put a new binary at /data/elite_ui (replacing the link)
rm -f "$D/elite_ui"; echo bin-new > "$D/elite_ui"
sh "$ROOT/init.sh" migrate
check '[ "$(cat $D/remote_ui)" = bin-new ] && [ -L $D/elite_ui ]' "a binary installed under the old name replaces remote_ui"
check '[ "$(cat $D/remote_settings.conf)" = settings ]' "settings untouched"

# 4. a new settings file is never overwritten by an old one
echo stale > "$D/elite_ui.conf"; echo current > "$D/remote_ui.conf"
sh "$ROOT/init.sh" migrate
check '[ "$(cat $D/remote_ui.conf)" = current ]' "an existing new file is kept"

# 5. a fresh install: nothing to migrate, no links to missing files
reset
echo bin > "$D/remote_ui"
sh "$ROOT/init.sh" migrate
check '[ -L $D/elite_ui ] && [ ! -e $D/elite_settings.conf ] && [ ! -e $D/elite_sync ]' "fresh install: only the binary link"
check '[ ! -f $ROOT/tmp/remote-init.log ] || ! grep -q Migrated $ROOT/tmp/remote-init.log' "fresh install: nothing logged as migrated"

if [ "$FAIL" -ne 0 ]; then echo "[-] $FAIL migration checks failed"; exit 1; fi
echo "[+] All init.sh migrate checks passed"
