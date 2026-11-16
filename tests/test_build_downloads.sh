#!/bin/sh
# Build downloads are checked against a SHA-256 before use.
# build/build_busybox.sh runs from a scratch copy of the repository layout with a fake curl
# first in PATH (no network): a download that does not match, or an HTTP error page saved by
# an earlier run, must stop the build and leave no tarball under the final name. The CI and
# release workflows must check every Bootlin toolchain tarball with sha256sum.
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/repo/build" "$WORK/bin"
cp "$REPO_ROOT/build/build_busybox.sh" "$WORK/repo/build/"
cat > "$WORK/bin/curl" <<'FAKE'
#!/bin/sh
echo "curl $*" >> "$FAKE_LOG"
while [ $# -gt 0 ]; do case "$1" in -o) shift; out=$1 ;; esac; shift; done
printf '<html>404 Not Found</html>\n' > "$out"
exit 0
FAKE
chmod +x "$WORK/bin/curl"
TAR="$WORK/repo/build/tmp/busybox-1.36.1.tar.bz2"

# 1. A download that does not match its SHA-256
if PATH="$WORK/bin:$PATH" FAKE_LOG="$WORK/log" bash "$WORK/repo/build/build_busybox.sh" mips >"$WORK/out" 2>&1; then
    echo "FAIL: a download with the wrong SHA-256 did not stop the build"; exit 1
fi
[ ! -e "$TAR" ] || { echo "FAIL: the bad download was kept as $TAR"; exit 1; }
grep -q 'SHA-256' "$WORK/out" || { echo "FAIL: no word about the checksum"; cat "$WORK/out"; exit 1; }

# 2. An error page cached by an earlier run is not used: it is fetched again (and refused)
printf '<html>error</html>\n' > "$TAR"
: > "$WORK/log"
if PATH="$WORK/bin:$PATH" FAKE_LOG="$WORK/log" bash "$WORK/repo/build/build_busybox.sh" mips >"$WORK/out" 2>&1; then
    echo "FAIL: a cached file with the wrong SHA-256 was used"; exit 1
fi
grep -q '^curl ' "$WORK/log" || { echo "FAIL: the bad cached file was not fetched again"; exit 1; }
[ ! -e "$TAR" ] || { echo "FAIL: the bad cached file is still there"; exit 1; }

# 3. Every Bootlin download in the workflows is checked
for wf in "$REPO_ROOT/.github/workflows/ci.yml" "$REPO_ROOT/.github/workflows/release.yml"; do
    n_dl=$(grep -c 'toolchains.bootlin.com' "$wf")
    n_ck=$(grep -c 'toolchain.tar.bz2" | sha256sum -c -' "$wf")
    [ "$n_dl" -gt 0 ] && [ "$n_dl" = "$n_ck" ] || { echo "FAIL: $wf: $n_dl toolchain downloads, $n_ck checked"; exit 1; }
    if grep -q 'curl -sSL "https://toolchains.bootlin.com[^"]*" | tar' "$wf"; then
        echo "FAIL: $wf still pipes an unchecked toolchain download into tar"; exit 1
    fi
done

echo "PASS: build downloads are checked against their SHA-256"
