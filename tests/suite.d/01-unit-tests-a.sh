# Mock hardware, name escaping, remote UI engine and the first half of the C unit tests
# (moved from tests/run_mock_suite.sh; the suite sources this file)

# 2. Run Mock Hardware Low-Level Verification Test
echo "[-] Compiling & running test_mock_hardware..."
gcc -Wall -O2 \
    "$SCRIPT_DIR/test_mock_hardware.c" \
    -o "$BUILD_DIR/test_mock_hardware"

HARMONY_MOCK_DIR="/tmp/harmony_mock" \
LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" \
    "$BUILD_DIR/test_mock_hardware"

# 2b. Names never become markup (web UI escaping; the sanitizer at load and save)
python3 "$SCRIPT_DIR/test_ui_escape_scan.py"
python3 "$SCRIPT_DIR/test_name_sanitize.py"

# 3. Run Headless Remote UI Engine Mock Test
echo "[-] Compiling & running test_remote_mock_ui..."
gcc -Wall -O2 \
    -I"$REPO_ROOT/payload/source" \
    -I"$REPO_ROOT/payload/source/remote" \
    "$SCRIPT_DIR/test_remote_mock_ui.c" \
    "$REPO_ROOT/payload/source/remote/remote_config.c" \
    "$REPO_ROOT/payload/source/remote/remote_fb.c" \
    "$REPO_ROOT/payload/source/remote/remote_i18n.c" \
    "$REPO_ROOT/payload/source/remote/remote_input.c" \
    "$REPO_ROOT/payload/source/remote/remote_power.c" \
    "$REPO_ROOT/payload/source/remote/remote_battery.c" \
    "$REPO_ROOT/payload/source/remote/remote_flightlog.c" \
    "$REPO_ROOT/payload/source/remote/remote_present.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf_ota.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf_sync.c" \
    "$REPO_ROOT/payload/source/remote/remote_state.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_activities.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_devices.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_pairing.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_menu.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_help.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_activity.c" \
    "$REPO_ROOT/payload/source/remote/remote_link.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_settings.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_settings_actions.c" \
    "$REPO_ROOT/payload/source/remote/remote_theme.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_scroll.c" \
    "$REPO_ROOT/payload/source/remote/remote_transition.c" \
    "$REPO_ROOT/payload/source/remote/remote_nav.c" \
    "$REPO_ROOT/payload/source/remote_stock_icons.c" \
    "$REPO_ROOT/payload/source/cJSON.c" \
    "$REPO_ROOT/payload/source/sha256.c" \
    "$REPO_ROOT/payload/source/timezone_db.c" \
    -lpthread -lm \
    -o "$BUILD_DIR/test_remote_mock_ui"

HARMONY_MOCK_DIR="/tmp/harmony_mock" \
LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" \
    "$BUILD_DIR/test_remote_mock_ui"

# Release of a key held over RF while an overlay is up
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" \
    "$SCRIPT_DIR/test_remote_key_release.c" \
    "$REPO_ROOT/payload/source/remote/remote_config.c" \
    "$REPO_ROOT/payload/source/remote/remote_fb.c" \
    "$REPO_ROOT/payload/source/remote/remote_i18n.c" \
    "$REPO_ROOT/payload/source/remote/remote_input.c" \
    "$REPO_ROOT/payload/source/remote/remote_power.c" \
    "$REPO_ROOT/payload/source/remote/remote_battery.c" \
    "$REPO_ROOT/payload/source/remote/remote_flightlog.c" \
    "$REPO_ROOT/payload/source/remote/remote_present.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf_ota.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf_sync.c" \
    "$REPO_ROOT/payload/source/remote/remote_state.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_activities.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_devices.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_pairing.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_menu.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_help.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_activity.c" \
    "$REPO_ROOT/payload/source/remote/remote_link.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_settings.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_settings_actions.c" \
    "$REPO_ROOT/payload/source/remote/remote_theme.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_scroll.c" \
    "$REPO_ROOT/payload/source/remote/remote_transition.c" \
    "$REPO_ROOT/payload/source/remote/remote_nav.c" \
    "$REPO_ROOT/payload/source/remote_stock_icons.c" \
    "$REPO_ROOT/payload/source/cJSON.c" \
    "$REPO_ROOT/payload/source/sha256.c" \
    "$REPO_ROOT/payload/source/timezone_db.c" \
    -lpthread -lm -o "$BUILD_DIR/test_remote_key_release" && "$BUILD_DIR/test_remote_key_release"
# Remote: key event counter in every key frame (payload[19], 1..255 then 1), in the order written
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_key_counter" "$SCRIPT_DIR/test_remote_key_counter.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && "$BUILD_DIR/test_remote_key_counter"

# The remote's main loop (the real remote_main.c) on the mock: a hard key pressed with the screen
# off is sent before the screen is lit
KEY_WAKE_SRCS=()
for f in "$REPO_ROOT"/payload/source/remote/*.c; do
    [ "${f##*/}" = "remote_main.c" ] || KEY_WAKE_SRCS+=("$f")
done
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" \
    "$SCRIPT_DIR/test_remote_key_wake.c" "${KEY_WAKE_SRCS[@]}" \
    "$REPO_ROOT/payload/source/remote_stock_icons.c" \
    "$REPO_ROOT/payload/source/mfg_id.c" \
    "$REPO_ROOT/payload/source/cJSON.c" \
    "$REPO_ROOT/payload/source/sha256.c" \
    "$REPO_ROOT/payload/source/timezone_db.c" \
    -Wl,--wrap=system,--wrap=open,--wrap=settimeofday,--wrap=write,--wrap=ioctl,--wrap=render_screen \
    -lpthread -lm -o "$BUILD_DIR/test_remote_key_wake"
KEY_WAKE_DIR="$(mktemp -d /tmp/hng_keywake.XXXXXX)"
HARMONY_MOCK_DIR="$KEY_WAKE_DIR" HARMONY_MOCK_ROLE=remote LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_key_wake"
rm -rf "$KEY_WAKE_DIR"

# 4. Run Core Component Unit Tests
echo "[-] Running core component unit tests..."
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_activity_icons" "$SCRIPT_DIR/test_activity_icons.c" && "$BUILD_DIR/test_activity_icons"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_remote_i18n" "$SCRIPT_DIR/test_remote_i18n.c" "$REPO_ROOT/payload/source/remote/remote_i18n.c" && "$BUILD_DIR/test_remote_i18n"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" \
    "$SCRIPT_DIR/test_remote_wdt_race.c" \
    "$REPO_ROOT/payload/source/remote/remote_config.c" \
    "$REPO_ROOT/payload/source/remote/remote_fb.c" \
    "$REPO_ROOT/payload/source/remote/remote_i18n.c" \
    "$REPO_ROOT/payload/source/remote/remote_input.c" \
    "$REPO_ROOT/payload/source/remote/remote_power.c" \
    "$REPO_ROOT/payload/source/remote/remote_battery.c" \
    "$REPO_ROOT/payload/source/remote/remote_flightlog.c" \
    "$REPO_ROOT/payload/source/remote/remote_present.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf_ota.c" \
    "$REPO_ROOT/payload/source/remote/remote_rf_sync.c" \
    "$REPO_ROOT/payload/source/remote/remote_state.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_activities.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_devices.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_pairing.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_menu.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_help.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_activity.c" \
    "$REPO_ROOT/payload/source/remote/remote_link.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_settings.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_settings_actions.c" \
    "$REPO_ROOT/payload/source/remote/remote_theme.c" \
    "$REPO_ROOT/payload/source/remote/remote_ui_scroll.c" \
    "$REPO_ROOT/payload/source/remote/remote_transition.c" \
    "$REPO_ROOT/payload/source/remote/remote_nav.c" \
    "$REPO_ROOT/payload/source/remote_stock_icons.c" \
    "$REPO_ROOT/payload/source/cJSON.c" \
    "$REPO_ROOT/payload/source/sha256.c" \
    "$REPO_ROOT/payload/source/timezone_db.c" \
    -ldl -lpthread -lm -o "$BUILD_DIR/test_remote_wdt_race" && "$BUILD_DIR/test_remote_wdt_race"
sh "$SCRIPT_DIR/test_remote_migrate.sh"
# Remote Help: a state stamp from the RF thread newer than the main loop's now_ms
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_help_clock" "$SCRIPT_DIR/test_remote_help_clock.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_help_clock"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_migrate" "$SCRIPT_DIR/test_remote_migrate.c" "$REPO_ROOT/payload/source/remote/remote_migrate.c" && "$BUILD_DIR/test_remote_migrate"
sh "$SCRIPT_DIR/test_remote_supervisor.sh"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_battery" "$SCRIPT_DIR/test_remote_battery.c" "$REPO_ROOT/payload/source/remote/remote_battery.c" && "$BUILD_DIR/test_remote_battery"
# Remote ACTIVITY_SYNC decoder: the name stays inside its field (frames at the end of a heap block)
build_test -Wall -O2 -fsanitize=address -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_activity_sync" "$SCRIPT_DIR/test_remote_activity_sync.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_remote_activity_sync" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_activity_sync"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_flightlog" "$SCRIPT_DIR/test_remote_flightlog.c" "$REPO_ROOT/payload/source/remote/remote_flightlog.c" && "$BUILD_DIR/test_remote_flightlog"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_custom_icon_registry" "$SCRIPT_DIR/test_custom_icon_registry.c" "$REPO_ROOT/payload/source/custom_icon_registry.c" "$REPO_ROOT/payload/source/cJSON.c" "$REPO_ROOT/payload/source/sha256.c" -lm && LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_custom_icon_registry"
# Config files replaced through a synced temporary file; a full flash keeps the old file
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_atomic_file" "$SCRIPT_DIR/test_atomic_file.c" && "$BUILD_DIR/test_atomic_file"
# Remote: custom icon updates free and reuse their PNG cache slots
build_test -Wall -O2 -fsanitize=address -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_png_cache" "$SCRIPT_DIR/test_remote_png_cache.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_remote_png_cache" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_png_cache"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sanitize_activity_steps" "$SCRIPT_DIR/test_sanitize_activity_steps.c" "$REPO_ROOT/payload/source/config_sanitize.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_sanitize_activity_steps"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sha256" "$SCRIPT_DIR/test_sha256.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_sha256"
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sntp_reply" "$SCRIPT_DIR/test_sntp_reply.c" "$REPO_ROOT/payload/source/hub_ntp.c" && "$BUILD_DIR/test_sntp_reply"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mqtt_connect_flags" "$SCRIPT_DIR/test_mqtt_connect_flags.c" "$REPO_ROOT/payload/source/mqtt_client.c" && "$BUILD_DIR/test_mqtt_connect_flags"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_encoder" "$SCRIPT_DIR/test_ir_encoder.c" "$REPO_ROOT/payload/source/ir_encoder.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_encoder"
# Remote: images too large in pixels are refused before they are decoded
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_png_pixel_limit" "$SCRIPT_DIR/test_remote_png_pixel_limit.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_png_pixel_limit"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_button_list" "$SCRIPT_DIR/test_rf_button_list.c" "$REPO_ROOT/payload/source/rf_button_list.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_rf_button_list"
build_test -Wall -O2 -fsanitize=address -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_keymap_lock" "$SCRIPT_DIR/test_rf_keymap_lock.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
# Remote: a redraw requested while a frame is drawn (fb_present wrapped by the test)
build_test -Wall -O2 -Wl,--wrap=fb_present -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_redraw_request" "$SCRIPT_DIR/test_remote_redraw_request.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_redraw_request"
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_rf_keymap_lock" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_keymap_lock"
