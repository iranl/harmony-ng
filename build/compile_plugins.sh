#!/bin/bash
# The plugins live in harmony-ng-plugins (next to this repo): its build_plugins.sh builds them
# with this repo's MIPS toolchain and BearSSL. Kept so the old entry point still works.
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLUGINS_ROOT="${PLUGINS_ROOT:-$REPO_ROOT/../harmony-ng-plugins}"
HARMONY_REPO_ROOT="$REPO_ROOT" exec bash "$PLUGINS_ROOT/build_plugins.sh" "$@"
