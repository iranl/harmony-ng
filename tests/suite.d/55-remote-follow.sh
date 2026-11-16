# hub_rf key fallback to another activity's mapping (and the web editor's preview of it); the
# remote follows a Hub activity change only on its home and activity screens; no raw touch I2C

# A key the current activity does not map: "-1", "default", then only the single activity of a map
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_keymap_fallback" \
    "$SCRIPT_DIR/test_rf_keymap_fallback.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" \
    -Wl,--wrap=fopen -lpthread -lm
"$BUILD_DIR/test_rf_keymap_fallback"
if command -v node >/dev/null 2>&1; then node "$SCRIPT_DIR/test_frontend_key_fallback.mjs"; else echo "    (node not found, test_frontend_key_fallback.mjs skipped)"; fi

# The remote follows a Hub activity change only on home and activity screens; Settings, pairing
# and the device browser stay, and a switch started on the remote ends on its target screen
FOLLOW_SRCS=("$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c)
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_activity_follow" \
    "$SCRIPT_DIR/test_remote_activity_follow.c" "${FOLLOW_SRCS[@]}" -lpthread -lm
FOLLOW_DIR="$(mktemp -d /tmp/hng_activity_follow.XXXXXX)"
HARMONY_MOCK_DIR="$FOLLOW_DIR" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_activity_follow" \
    || { rm -rf "$FOLLOW_DIR"; exit 1; }
rm -rf "$FOLLOW_DIR"

# The touch controller is left to its kernel driver: no raw I2C at a blank or a wake, and no
# remote source or boot script opens or creates /dev/i2c-1
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_no_raw_i2c" \
    "$SCRIPT_DIR/test_remote_no_raw_i2c.c" "${FOLLOW_SRCS[@]}" -Wl,--wrap=open -lpthread -lm
NO_I2C_DIR="$(mktemp -d /tmp/hng_no_raw_i2c.XXXXXX)"
HARMONY_MOCK_DIR="$NO_I2C_DIR" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_no_raw_i2c" \
    || { rm -rf "$NO_I2C_DIR"; exit 1; }
rm -rf "$NO_I2C_DIR"
# Text files only, tracked or new, not ignored build output (remote/bin holds a locally built
# BusyBox with i2c applets)
if git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    RAW_I2C_HITS="$(git -C "$REPO_ROOT" grep --untracked -n -I -e '/dev/i2c' -e 'I2C_SLAVE' -- payload/source/remote remote || true)"
else
    RAW_I2C_HITS="$(grep -rnI --exclude-dir=bin -e '/dev/i2c' -e 'I2C_SLAVE' "$REPO_ROOT/payload/source/remote" "$REPO_ROOT/remote" || true)"
fi
if [ -n "$RAW_I2C_HITS" ]; then
    echo "$RAW_I2C_HITS"
    echo "[-] remote code still uses a raw I2C bus node (above)"
    exit 1
fi
echo "[+] no raw I2C bus node in the remote sources and scripts"
