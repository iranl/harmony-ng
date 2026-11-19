#!/bin/bash
set -euo pipefail

# "gcc ... && ./test" does not stop the suite when gcc fails: under set -e only the last command
# of an && list counts, so a test that no longer compiled was skipped and the suite still passed.
# -Werror=implicit-function-declaration: a call without a prototype (a missing #include) is an
# error on gcc 14 and newer, only a warning on older gcc; make it an error on every compiler.
build_test() { gcc -Werror=implicit-function-declaration "$@" || { echo "[-] A test did not compile (gcc error above)"; exit 1; }; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/tmp/test_host"

mkdir -p "$BUILD_DIR"

echo "==========================================================="
echo "  Harmony Automated Full-Stack User-Space Test Suite       "
echo "==========================================================="

# 1. Compile Mock Hardware Interceptor Shared Library
echo "[-] Compiling libmock_hardware.so..."
gcc -Wall -O2 -fPIC -shared \
    "$SCRIPT_DIR/mock_hardware/mock_hardware.c" \
    -ldl -o "$BUILD_DIR/libmock_hardware.so"

# 2. The tests, in group files sourced in name order (tests/suite.d/*.sh). A new group adds a
# file there instead of a line here, so branches do not touch this script.
# Parts named *host-tools* test the host tools and run in tests/run_host_tools.sh instead.
# SUITE_GROUP=1, 2, 3 or 4 runs one quarter of the groups (CI runs the four quarters on separate
# machines at the same time); without it every group runs. The two digits that start a group
# file's name pick its quarter: 00-24 is 1, 25-49 is 2, 50-74 is 3, 75-99 is 4.
SUITE_GROUP="${SUITE_GROUP:-}"
case "$SUITE_GROUP" in
    ""|1|2|3|4) ;;
    *) echo "[-] SUITE_GROUP must be 1, 2, 3 or 4 (or unset for all)"; exit 1 ;;
esac
for suite_part in "$SCRIPT_DIR"/suite.d/*.sh; do
    [ -e "$suite_part" ] || continue
    part_name=$(basename "$suite_part" .sh)
    case "$part_name" in *host-tools*) continue ;; esac
    case "$part_name" in
        [0-9][0-9]-*) ;;
        *) echo "[-] tests/suite.d/$part_name.sh: a test group file name starts with two digits"; exit 1 ;;
    esac
    if [ -n "$SUITE_GROUP" ] && [ $(( 10#${part_name%%-*} / 25 + 1 )) -ne "$SUITE_GROUP" ]; then
        continue
    fi
    echo "[-] Test group $part_name"
    . "$suite_part"
done

echo "==========================================================="
if [ -n "$SUITE_GROUP" ]; then
    echo "  GROUP $SUITE_GROUP OF 4: ALL TESTS COMPLETED WITH 100% PASS"
else
    echo "  ALL MOCKING & AUTOMATED TESTS COMPLETED WITH 100% PASS   "
fi
echo "==========================================================="
