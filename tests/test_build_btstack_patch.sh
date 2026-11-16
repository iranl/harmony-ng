#!/bin/sh
# build/build_btstack.sh applies btstack.patch to a clean BTstack clone on every build.
# A clone left by an earlier build must not keep a change that was removed from the patch,
# and a patch that does not apply, or is missing, must stop the build. The script's patch
# step runs here on a toy git repository (no network, no toolchain).
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

sed -n '/^# >>> patch step/,/^# <<< patch step/p' "$REPO_ROOT/build/build_btstack.sh" > "$WORK/step.sh"
[ -s "$WORK/step.sh" ] || { echo "FAIL: patch step markers not found in build_btstack.sh"; exit 1; }

BTSTACK_DIR="$WORK/btstack"
mkdir -p "$BTSTACK_DIR"
cd "$BTSTACK_DIR"
git init -q
printf 'a1\n' > a.c
printf 'b1\n' > b.c
git add a.c b.c
git -c user.name=t -c user.email=t@example.invalid commit -q -m base
for f in a b; do cp $f.c $f.orig; done
printf 'a2 patched\n' > a.c
printf 'b2 patched\n' > b.c
git diff > "$WORK/old.patch"
printf 'a2 patched\n' > a.c
printf 'b1\n' > b.c
git diff > "$WORK/new.patch"   # the b.c change was removed from the patch
git checkout -q -- .
rm -f a.orig b.orig
cd "$WORK"

run_step() {
    BTSTACK_DIR="$BTSTACK_DIR" BTSTACK_PATCH="$1" BTSTACK_COMMIT=test sh "$WORK/step.sh"
}

run_step "$WORK/old.patch" >/dev/null
[ "$(cat "$BTSTACK_DIR/b.c")" = "b2 patched" ] || { echo "FAIL: first build did not apply the patch"; exit 1; }

run_step "$WORK/new.patch" >/dev/null
if [ "$(cat "$BTSTACK_DIR/b.c")" != "b1" ]; then
    echo "FAIL: a change removed from btstack.patch stayed in the clone that is compiled"
    exit 1
fi
[ "$(cat "$BTSTACK_DIR/a.c")" = "a2 patched" ] || { echo "FAIL: the new patch is not applied"; exit 1; }

# A second build with the same patch works too (clean tree, patch applied again)
run_step "$WORK/new.patch" >/dev/null || { echo "FAIL: rebuild with the same patch"; exit 1; }

printf 'not a patch\n' > "$WORK/bad.patch"
if run_step "$WORK/bad.patch" >/dev/null 2>&1; then echo "FAIL: a patch that does not apply did not stop the build"; exit 1; fi
if run_step "$WORK/missing.patch" >/dev/null 2>&1; then echo "FAIL: a missing patch did not stop the build"; exit 1; fi

echo "PASS: btstack.patch is applied to a clean clone"
