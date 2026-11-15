# hub_rf key fallback to another activity's mapping; the remote follows a Hub activity change only
# on its home and activity screens

# A key the current activity does not map: "-1", "default", then only the single activity of a map
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_keymap_fallback" \
    "$SCRIPT_DIR/test_rf_keymap_fallback.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" \
    -Wl,--wrap=fopen -lpthread -lm
"$BUILD_DIR/test_rf_keymap_fallback"

# The remote follows a Hub activity change only on home and activity screens; Settings, pairing
# and the device browser stay, and a switch started on the remote ends on its target screen
FOLLOW_SRCS=("$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c)
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_activity_follow" \
    "$SCRIPT_DIR/test_remote_activity_follow.c" "${FOLLOW_SRCS[@]}" -lpthread -lm
FOLLOW_DIR="$(mktemp -d /tmp/hng_activity_follow.XXXXXX)"
HARMONY_MOCK_DIR="$FOLLOW_DIR" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_activity_follow"
rm -rf "$FOLLOW_DIR"
