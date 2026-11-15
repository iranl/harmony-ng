#!/bin/sh
# The binaries committed to the repository match the SHA-256 list in tools/bin/README.md, and
# every committed binary of that kind is listed. A replaced blob must update the list (with
# its source and build options) in the same commit.
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
README="$REPO_ROOT/tools/bin/README.md"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$REPO_ROOT"

# Table rows: | `path` | ... | `sha` | ...  and the module block: "sha  name.ko"
sed -n 's/^| `\([^`]*\)` |.*| `\([0-9a-f]\{64\}\)` |.*/\2  \1/p' "$README" > "$WORK/list"
sed -n 's#^\([0-9a-f]\{64\}\)  \([A-Za-z0-9_]*\.ko\)$#\1  payload/modules/\2#p' "$README" >> "$WORK/list"
sha256sum -c --quiet "$WORK/list" || { echo "FAIL: a committed binary differs from tools/bin/README.md"; exit 1; }

for f in tools/bin/dropbearmulti_mips remote/bin/dropbearmulti_musl remote/b64dec_tiny $(git ls-files 'payload/modules/*.ko'); do
    grep -q "  $f\$" "$WORK/list" || { echo "FAIL: $f is not listed in tools/bin/README.md"; exit 1; }
done
echo "PASS: committed binaries match tools/bin/README.md ($(wc -l < "$WORK/list") files)"
