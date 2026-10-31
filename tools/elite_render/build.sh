#!/bin/bash
# Builds the screen renderer from the firmware's drawing code.
#   build.sh [OUT]          host CLI (default build/tmp/elite-render/elite_render)
#   build.sh --wasm [DIR]   WebAssembly module and its loader (default build/tmp/elite-render/web,
#                           which also gets the development page); the web UIs use
#                           android-app/public/assets and frontend/public/assets
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../payload/source"
BUILD="$HERE/../../build/tmp/elite-render"
FIRMWARE=(
    "$SRC/elite/elite_fb.c" "$SRC/elite/elite_state.c" "$SRC/elite/elite_config.c"
    "$SRC/elite/elite_ui.c" "$SRC/elite/elite_ui_activities.c" "$SRC/elite/elite_ui_devices.c"
    "$SRC/elite/elite_ui_settings.c" "$SRC/elite/elite_ui_settings_actions.c"
    "$SRC/elite/elite_ui_pairing.c" "$SRC/elite/elite_ui_menu.c" "$SRC/elite/elite_ui_activity.c" "$SRC/elite/elite_rf_ota.c" "$SRC/elite/elite_i18n.c"
    "$SRC/elite_stock_icons.c" "$SRC/sha256.c" "$SRC/cJSON.c"
    "$HERE/host_platform.c" "$HERE/render_state.c"
)
FLAGS=(-O2 -Wall -Wno-unused-function -I"$SRC" -I"$SRC/elite" -I"$HERE")

if [ "${1:-}" = "--wasm" ]; then
    OUT="${2:-$BUILD/web}"
    mkdir -p "$OUT"
    [ -z "${2:-}" ] && cp "$HERE/web/index.html" "$OUT/"
    emcc "${FLAGS[@]}" -I"$HERE/wasm_compat" "$HERE/elite_render_wasm.c" "${FIRMWARE[@]}" \
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createEliteRenderer -sENVIRONMENT=web \
        -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_FUNCTIONS=_main,_er_render,_er_assets_changed,_er_icon_keyword,_er_stock_icon_id \
        -sEXPORTED_RUNTIME_METHODS=ccall,FS,HEAPU8 \
        -o "$OUT/elite_render.js"
    cp "$HERE/web/elite_preview.js" "$OUT/"
    echo "$OUT"
else
    OUT="${1:-$BUILD/elite_render}"
    mkdir -p "$(dirname "$OUT")"
    ${CC:-cc} "${FLAGS[@]}" "$HERE/elite_render.c" "${FIRMWARE[@]}" -lm -lpthread -o "$OUT"
    echo "$OUT"
fi
