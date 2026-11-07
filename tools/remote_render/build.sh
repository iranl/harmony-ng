#!/bin/bash
# Builds the screen renderer from the firmware's drawing code.
#   build.sh [OUT]          host CLI (default build/tmp/remote-render/remote_render)
#   build.sh --wasm [DIR]   WebAssembly module and its loader (default build/tmp/remote-render/web,
#                           which also gets the development page); the web UIs use
#                           android-app/public/assets and frontend/public/assets
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../payload/source"
BUILD="$HERE/../../build/tmp/remote-render"
FIRMWARE=(
    "$SRC/remote/remote_fb.c" "$SRC/remote/remote_state.c" "$SRC/remote/remote_config.c"
    "$SRC/remote/remote_ui.c" "$SRC/remote/remote_ui_activities.c" "$SRC/remote/remote_ui_devices.c"
    "$SRC/remote/remote_ui_settings.c" "$SRC/remote/remote_ui_settings_actions.c"
    "$SRC/remote/remote_ui_pairing.c" "$SRC/remote/remote_ui_menu.c" "$SRC/remote/remote_ui_help.c" "$SRC/remote/remote_ui_activity.c" "$SRC/remote/remote_link.c" "$SRC/remote/remote_rf_ota.c" "$SRC/remote/remote_i18n.c" "$SRC/remote/remote_theme.c" "$SRC/remote/remote_ui_scroll.c" "$SRC/remote/remote_transition.c"
    "$SRC/remote_stock_icons.c" "$SRC/sha256.c" "$SRC/cJSON.c"
    "$HERE/host_platform.c" "$HERE/render_state.c"
)
FLAGS=(-O2 -Wall -Wno-unused-function -I"$SRC" -I"$SRC/remote" -I"$HERE")

if [ "${1:-}" = "--wasm" ]; then
    OUT="${2:-$BUILD/web}"
    mkdir -p "$OUT"
    [ -z "${2:-}" ] && cp "$HERE/web/index.html" "$OUT/"
    emcc "${FLAGS[@]}" -I"$HERE/wasm_compat" "$HERE/remote_render_wasm.c" "${FIRMWARE[@]}" \
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createRemoteRenderer -sENVIRONMENT=web \
        -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_FUNCTIONS=_main,_er_render,_er_assets_changed,_er_icon_keyword,_er_stock_icon_id,_er_activity_icon,_er_device_icon,_er_hit,_er_list_top,_er_home_page_size \
        -sEXPORTED_RUNTIME_METHODS=ccall,FS,HEAPU8 \
        -o "$OUT/remote_render.js"
    cp "$HERE/web/remote_preview.js" "$OUT/"
    echo "$OUT"
else
    OUT="${1:-$BUILD/remote_render}"
    mkdir -p "$(dirname "$OUT")"
    ${CC:-cc} "${FLAGS[@]}" "$HERE/remote_render.c" "${FIRMWARE[@]}" -lm -lpthread -o "$OUT"
    echo "$OUT"
fi
