# Remote power, watchdog and shell scripts; remote UI screens and state

REMOTE_TEST_SRCS=("$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c)

# Suspend gates, the reboot decision after leaving the cradle, the last look before a suspend,
# SIGUSR2 and a clock step (power thread on the mock; no helper, exec or clock change on the host)
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_power_paths" \
    "$SCRIPT_DIR/test_remote_power_paths.c" "${REMOTE_TEST_SRCS[@]}" \
    -Wl,--wrap=system,--wrap=execv,--wrap=settimeofday,--wrap=time -ldl -lpthread -lm
POWER_PATHS_DIR="$(mktemp -d /tmp/hng_power_paths.XXXXXX)"
HARMONY_MOCK_DIR="$POWER_PATHS_DIR" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_power_paths"
rm -rf "$POWER_PATHS_DIR"

# The crash handler in remote_main.c uses async-signal-safe calls only
CRASH_SRCS=()
for f in "$REPO_ROOT"/payload/source/remote/*.c; do
    [ "${f##*/}" = "remote_main.c" ] || CRASH_SRCS+=("$f")
done
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_crash_handler" \
    "$SCRIPT_DIR/test_remote_crash_handler.c" "${CRASH_SRCS[@]}" \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,mfg_id,cJSON,sha256,timezone_db}.c \
    -Wl,--wrap=fopen,--wrap=localtime,--wrap=system -lpthread -lm
CRASH_DIR="$(mktemp -d /tmp/hng_crash.XXXXXX)"
HARMONY_MOCK_DIR="$CRASH_DIR" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_crash_handler"
rm -rf "$CRASH_DIR"

# Settings, Hub config and usage files: a write cut short keeps the old file; settings range checks
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_settings_files" \
    "$SCRIPT_DIR/test_remote_settings_files.c" "${REMOTE_TEST_SRCS[@]}" -lpthread -lm
SETTINGS_FILES_DIR="$(mktemp -d /tmp/hng_settings_files.XXXXXX)"
HARMONY_MOCK_DIR="$SETTINGS_FILES_DIR" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_settings_files"
rm -rf "$SETTINGS_FILES_DIR"
