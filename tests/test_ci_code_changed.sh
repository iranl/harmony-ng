#!/bin/sh
# .github/scripts/code_changed.sh: CI skips the builds and firmware tests only for a change made
# of Markdown files that no test names. Runs in a scratch tree with its own tests/ folder.
# (File names are built from two parts so that this file does not name them itself.)
set -u
SCRIPT="$(cd "$(dirname "$0")/.." && pwd)/.github/scripts/code_changed.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tests"
read_doc="docs/READ_BY""_A_TEST.md"
prose="docs/PROSE""_ONLY.md"
echo "open(\"$read_doc\")" > "$work/tests/test_reads_a_doc.py"

fail=0
check() {
    want=$1; label=$2; shift 2
    got=$(cd "$work" && printf '%s\n' "$@" | sh "$SCRIPT" 2>/dev/null)
    if [ "$got" = "$want" ]; then echo "  ok   $label"; else echo "  FAIL $label: got '$got', want '$want'"; fail=1; fi
}
echo "CI code_changed.sh"
check false "one prose file" "$prose"
check false "prose files only, also at the root" "$prose" "OTHER""_PROSE.md"
check true  "a Markdown file a test reads" "$read_doc"
check true  "prose plus a source file" "$prose" "payload/source/hub_daemon.c"
check true  "prose plus a workflow" "$prose" ".github/workflows/ci.yml"
check true  "a source file" "payload/source/rf_main.c"
check true  "nothing listed" ""
check true  "a Markdown file under tests/" "tests/fixtures/x""y.md" 
[ "$fail" -eq 0 ] && echo "test_ci_code_changed: all checks passed"
exit "$fail"
