#!/bin/bash
set -euo pipefail

# "gcc ... && ./test" does not stop the suite when gcc fails: under set -e only the last command
# of an && list counts, so a test that no longer compiled was skipped and the suite still passed.
build_test() { gcc "$@" || { echo "[-] A test did not compile (gcc error above)"; exit 1; }; }

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
# Host tool with a fake ssh (tools/backup_hub.py keeps only complete tar archives)
python3 "$SCRIPT_DIR/test_backup_hub_check.py"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_custom_icon_registry" "$SCRIPT_DIR/test_custom_icon_registry.c" "$REPO_ROOT/payload/source/custom_icon_registry.c" "$REPO_ROOT/payload/source/cJSON.c" "$REPO_ROOT/payload/source/sha256.c" -lm && LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_custom_icon_registry"
# Remote: custom icon updates free and reuse their PNG cache slots
build_test -Wall -O2 -fsanitize=address -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_png_cache" "$SCRIPT_DIR/test_remote_png_cache.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_remote_png_cache" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_png_cache"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sanitize_activity_steps" "$SCRIPT_DIR/test_sanitize_activity_steps.c" "$REPO_ROOT/payload/source/config_sanitize.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_sanitize_activity_steps"
# Host tool with the device calls replaced (remote/setup_remote.py --no-screen writes remote_settings.conf)
python3 "$SCRIPT_DIR/test_setup_remote_no_screen.py"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sha256" "$SCRIPT_DIR/test_sha256.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_sha256"
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sntp_reply" "$SCRIPT_DIR/test_sntp_reply.c" "$REPO_ROOT/payload/source/hub_ntp.c" && "$BUILD_DIR/test_sntp_reply"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mqtt_connect_flags" "$SCRIPT_DIR/test_mqtt_connect_flags.c" "$REPO_ROOT/payload/source/mqtt_client.c" && "$BUILD_DIR/test_mqtt_connect_flags"
# Host tool in the release bundle layout (remote/setup_remote.py finds init.sh and remote_watchdog.sh)
python3 "$SCRIPT_DIR/test_setup_remote_bundle.py"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_encoder" "$SCRIPT_DIR/test_ir_encoder.c" "$REPO_ROOT/payload/source/ir_encoder.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_encoder"
# Remote: images too large in pixels are refused before they are decoded
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_png_pixel_limit" "$SCRIPT_DIR/test_remote_png_pixel_limit.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_png_pixel_limit"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_button_list" "$SCRIPT_DIR/test_rf_button_list.c" "$REPO_ROOT/payload/source/rf_button_list.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_rf_button_list"
# Host tool (tools/migrate_hub_configs.py keeps tar.gz members inside its extraction folder)
python3 "$SCRIPT_DIR/test_migrate_tar_paths.py"
build_test -Wall -O2 -fsanitize=address -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_keymap_lock" "$SCRIPT_DIR/test_rf_keymap_lock.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
# Remote: a redraw requested while a frame is drawn (fb_present wrapped by the test)
build_test -Wall -O2 -Wl,--wrap=fb_present -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_redraw_request" "$SCRIPT_DIR/test_remote_redraw_request.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_redraw_request"
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_rf_keymap_lock" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_keymap_lock"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_companion_timeout" "$SCRIPT_DIR/test_rf_companion_timeout.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_companion_timeout"
# A companion key press is never seen half stored (strncpy wrapped to widen the window)
build_test -Wall -O2 -fno-builtin-strncpy -Wl,--wrap=strncpy -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_companion_race" "$SCRIPT_DIR/test_rf_companion_race.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_companion_race"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_capture" "$SCRIPT_DIR/test_ir_capture.c" "$REPO_ROOT/payload/source/ir_encoder.c" "$REPO_ROOT/payload/source/ir_i2s.c" -lpthread && "$BUILD_DIR/test_ir_capture" "$SCRIPT_DIR/fixtures/ir/samsung36_volume_up.i2s"
# Remote: an activity change reported right after a wake that follows a suspend
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_wake_catchup" "$SCRIPT_DIR/test_remote_wake_catchup.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_wake_catchup"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_action_lifecycle" "$SCRIPT_DIR/test_action_lifecycle.c" "$REPO_ROOT/payload/source/hw_action.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_action_lifecycle"
# Hub init.sh log cap in a host sandbox (every log in /tmp is capped)
python3 "$SCRIPT_DIR/test_init_log_cap.py"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_bt_state_migrate" "$SCRIPT_DIR/test_bt_state_migrate.c" "$REPO_ROOT/payload/source/bt_migrate.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_bt_state_migrate"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_hid_hex" "$SCRIPT_DIR/test_bt_hid_hex.c" && "$BUILD_DIR/test_bt_hid_hex"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mfg" "$SCRIPT_DIR/test_mfg.c" "$REPO_ROOT/payload/source/mfg_id.c" && "$BUILD_DIR/test_mfg"
# Host tool with a fake USB HID transport (tools/harmony_hid.py reports a cut file write)
python3 "$SCRIPT_DIR/test_hid_write_result.py"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ws_read_frame" "$SCRIPT_DIR/test_ws_read_frame.c" "$REPO_ROOT/payload/source/ws_server.c" -lpthread && "$BUILD_DIR/test_ws_read_frame"
# Remote: windowed OTA data frames that arrive outside OTA mode (real RF receive thread)
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_stray_ota_data" "$SCRIPT_DIR/test_remote_stray_ota_data.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_stray_ota_data"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mdns_packet" "$SCRIPT_DIR/test_mdns_packet.c" "$REPO_ROOT/payload/source/hub_mdns.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_mdns_packet"
# hub_rf config push targets (all hub_rf sources as build/compile_all.sh lists them, main renamed; /dev/rfspi is a socket pair)
build_test -Wall -O2 -fsanitize=address -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_push_targets" "$SCRIPT_DIR/test_rf_push_targets.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_rf_push_targets" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_push_targets"
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mqtt_client_io" "$SCRIPT_DIR/test_mqtt_client_io.c" "$REPO_ROOT/payload/source/mqtt_client.c" && "$BUILD_DIR/test_mqtt_client_io"
python3 "$SCRIPT_DIR/test_switched_marker.py"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_hbus_config" "$SCRIPT_DIR/test_hbus_config.c" "$REPO_ROOT/payload/source/hbus_config.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_hbus_config" "$SCRIPT_DIR/fixtures/ha/sample_config"
# Host tool with a fake ssh (tools/factory_reset.py stops and exits non-zero when a step fails)
python3 "$SCRIPT_DIR/test_factory_reset_tool.py"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_hub_events" "$SCRIPT_DIR/test_hub_events.c" "$REPO_ROOT/payload/source/hub_events.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_hub_events"
# hub_rf frame handler on a DEVICE_CMD_REQUEST with no screen button (all hub_rf sources as build/compile_all.sh lists them, main renamed)
build_test -Wall -O2 -fsanitize=address -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_device_cmd_frame" "$SCRIPT_DIR/test_rf_device_cmd_frame.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_rf_device_cmd_frame" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_device_cmd_frame"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_status_json" "$SCRIPT_DIR/test_rf_status_json.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_status_json"
# Host tool with ssh and npm replaced (build/deploy_hub.py starts the services again after an early end)
python3 "$SCRIPT_DIR/test_deploy_hub_restart.py"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_model" "$SCRIPT_DIR/test_rf_model.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_model"
# Host tool (tools/migrate_hub_configs.py keeps transport and isMacro on the codex and bundle paths)
python3 "$SCRIPT_DIR/test_migrate_keep_transport.py"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_battery" "$SCRIPT_DIR/test_rf_battery.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_battery"
# Remote: home selection and pending confirmation across a config push that rebuilds the list
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_selection_rebuild" "$SCRIPT_DIR/test_remote_selection_rebuild.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_selection_rebuild"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_rf_ota_resume" "$SCRIPT_DIR/test_rf_ota_resume.c" "$REPO_ROOT/payload/source/remote/remote_rf_ota.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_rf_ota_resume"
python3 "$SCRIPT_DIR/test_inventory_string_ids.py"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_migrate" "$SCRIPT_DIR/test_rf_migrate.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_migrate"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -DOTA_EXTRA_PUBKEY="$(cat "$SCRIPT_DIR/ota_test_pubkey.txt")" -o "$BUILD_DIR/test_rf_ota_lifecycle" "$SCRIPT_DIR/test_rf_ota_lifecycle.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_sync.c" "$REPO_ROOT/payload/source/rf_ota.c" "$REPO_ROOT/payload/source/ota_verify.c" "$REPO_ROOT/payload/source/tweetnacl.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_ota_lifecycle"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -DOTA_EXTRA_PUBKEY="$(cat "$SCRIPT_DIR/ota_test_pubkey.txt")" -o "$BUILD_DIR/test_ota_verify" "$SCRIPT_DIR/test_ota_verify.c" "$REPO_ROOT/payload/source/ota_verify.c" "$REPO_ROOT/payload/source/tweetnacl.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_ota_verify"

# hub_rf: frames from a CC2544 slot that holds no remote (all hub_rf sources as build/compile_all.sh lists them, main renamed)
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_foreign_slot" "$SCRIPT_DIR/test_rf_foreign_slot.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_foreign_slot"
# 4b. IR receiver: decoding a real recording, and a send while a stalled receiver is closing
gcc -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_rx_decode" "$SCRIPT_DIR/test_ir_rx_decode.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_rx_decode" "$SCRIPT_DIR/fixtures/ir/samsung36_volume_up.i2s"
build_test -Wall -O1 -g -fsanitize=address -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_blob" "$SCRIPT_DIR/test_ir_blob.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_blob"
gcc -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_tx_stall" "$SCRIPT_DIR/test_ir_tx_stall.c" "$REPO_ROOT/payload/source/ir_i2s.c" && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_ir_tx_stall"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_capture_mode_race" "$SCRIPT_DIR/test_ir_capture_mode_race.c" "$REPO_ROOT/payload/source/ir_i2s.c" && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_ir_capture_mode_race"

# 4c. hub_btstack reconnect queue and connected-hosts file
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_release_timers" "$SCRIPT_DIR/test_bt_release_timers.c" && "$BUILD_DIR/test_bt_release_timers"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_reconnect_queue" "$SCRIPT_DIR/test_bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" && "$BUILD_DIR/test_bt_reconnect_queue"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_le_pair_window" "$SCRIPT_DIR/test_bt_le_pair_window.c" "$REPO_ROOT/payload/source/hub_btstack/bt_le_pair_window.c" && "$BUILD_DIR/test_bt_le_pair_window"

# 4d. safe_reboot.sh in a host sandbox (supervisor loops stopped before reboot -f)
python3 "$SCRIPT_DIR/test_safe_reboot.py"
# update.sh apply in a host sandbox (a failed copy changes no installed file and no version)
python3 "$SCRIPT_DIR/test_update_apply.py"

# 5. Run Hub & Remote Full Co-Simulation
python3 "$SCRIPT_DIR/test_bt_connect_commands.py"
echo "[-] Launching Full Hub & Remote Co-Simulation..."
python3 "$SCRIPT_DIR/test_config_unreadable.py"
python3 "$SCRIPT_DIR/test_co_simulation.py"
# Host tool with a fake remote (remote/deploy_remote.py stops before the restart when an upload fails)
python3 "$SCRIPT_DIR/test_deploy_remote_failures.py"

# USB Ethernet adapters listed in the network guide reach a driver that binds them (start_usb_eth.sh in a host sandbox)
python3 "$SCRIPT_DIR/test_usb_eth_table.py"

# 5a. Bluetooth links around activity power on and off (orchestrator, daemon in --dry-run)
python3 "$SCRIPT_DIR/test_activity_test_sequence.py"
python3 "$SCRIPT_DIR/test_bt_power_link.py"
# Legacy web UI modules import the functions they call (no build step reports a missing import)
python3 "$SCRIPT_DIR/test_ui_imports.py"
python3 "$SCRIPT_DIR/test_activity_queue.py"
python3 "$SCRIPT_DIR/test_activity_restore.py"
python3 "$SCRIPT_DIR/test_activity_switch_timing.py"
python3 "$SCRIPT_DIR/test_activity_save_match.py"
python3 "$SCRIPT_DIR/test_config_cache_race.py"

# Hub boot scripts in a host sandbox (reboot counter, emergency recovery mode)
python3 "$SCRIPT_DIR/test_hub_boot_counter.py"

# 5b. WebSocket event bus tests against hub_daemon_test on the mock root
python3 "$SCRIPT_DIR/test_poweroff_activity.py"
echo "[-] Running WebSocket event tests..."
python3 "$SCRIPT_DIR/test_activity_repeat_request.py"
python3 "$SCRIPT_DIR/test_ws_events.py"
python3 "$SCRIPT_DIR/test_reentrant_libc.py"
python3 "$SCRIPT_DIR/test_status.py"
python3 "$SCRIPT_DIR/test_http_worker_timeouts.py"
python3 "$SCRIPT_DIR/test_static_cache_key.py"
python3 "$SCRIPT_DIR/test_ws_auth.py"
python3 "$SCRIPT_DIR/test_ws_slow_frame.py"
python3 "$SCRIPT_DIR/test_lan_request_guards.py"
python3 "$SCRIPT_DIR/test_ws_keepalive.py"
python3 "$SCRIPT_DIR/test_ws_slow_reader.py"
python3 "$SCRIPT_DIR/test_sntp_nonblocking.py"
python3 "$SCRIPT_DIR/test_ir_raw.py"
python3 "$SCRIPT_DIR/test_request_body_limit.py"
python3 "$SCRIPT_DIR/test_activity_help_answer.py"
python3 "$SCRIPT_DIR/test_mdns.py"
python3 "$SCRIPT_DIR/test_import_validate.py"
python3 "$SCRIPT_DIR/test_auth_hardening.py"
python3 "$SCRIPT_DIR/test_no_lan_responders.py"
# bt_reconnect.sh in a host sandbox (a stale pid file never kills another process)
python3 "$SCRIPT_DIR/test_bt_reconnect_pidfile.py"
python3 "$SCRIPT_DIR/test_mqtt_v2.py"
python3 "$SCRIPT_DIR/test_rf_key_channel.py"
# New web UI Wi-Fi form against a stubbed fetch (Node, no Hub)
if command -v node >/dev/null 2>&1; then node "$SCRIPT_DIR/test_frontend_wifi_save.mjs"; else echo "    (node not found, test_frontend_wifi_save.mjs skipped)"; fi
python3 "$SCRIPT_DIR/test_cmd_actions.py"
# Host tool with a fake USB device (tools/harmony_firmware.py flash flow)
python3 "$SCRIPT_DIR/test_harmony_firmware_flash.py"
python3 "$SCRIPT_DIR/test_rf_status_push.py"
# Web UI packaging (build/gzip_assets.py output does not change between builds of the same input)
python3 "$SCRIPT_DIR/test_gzip_assets_reproducible.py"
python3 "$SCRIPT_DIR/test_boot_time.py"
# USB root boot scripts in a host sandbox (refuse the other device class)
python3 "$SCRIPT_DIR/test_root_boot_scripts.py"
python3 "$SCRIPT_DIR/test_hbus.py"
# Host tool against a fake Hub root (tools/restore_stock.py, no network)
python3 "$SCRIPT_DIR/test_restore_stock.py"
python3 "$SCRIPT_DIR/test_ssdp.py"
# New web UI dialogs with a failing Hub call (Node, stand-in DOM, no Hub)
if command -v node >/dev/null 2>&1; then node "$SCRIPT_DIR/test_frontend_modal_errors.mjs"; else echo "    (node not found, test_frontend_modal_errors.mjs skipped)"; fi
python3 "$SCRIPT_DIR/test_hbus_aioharmony.py"
python3 "$SCRIPT_DIR/test_save_json_types.py"
python3 "$SCRIPT_DIR/test_icon_proxy_limits.py"
python3 "$SCRIPT_DIR/test_device_delay_limits.py"

# 5c. Host tools (no device contact)
# setup_remote.py SSH key pair names (ssh-keygen replaced)
python3 "$SCRIPT_DIR/test_host_tools_ssh.py"
python3 "$SCRIPT_DIR/test_deploy_hub_space.py"
# Web UI log viewers offer only sources GET /api/logs serves
python3 "$SCRIPT_DIR/test_webui_log_sources.py"

# Host tools stop with a clear message on Python older than 3.11 (docs/FIRST_INSTALL_*.md)
python3 "$SCRIPT_DIR/test_python_minimum.py"

# 6. Run OpenAPI Specification Contract Validation (docs/openapi.yaml must parse as YAML)
python3 "$SCRIPT_DIR/test_ethernet_apply.py"
echo "[-] Validating docs/openapi.yaml with PyYAML..."
python3 "$SCRIPT_DIR/test_mqtt_device_command.py"
python3 -c "import yaml, sys; yaml.safe_load(open('$REPO_ROOT/docs/openapi.yaml')); print('    openapi.yaml parses')"
# Every C file in payload/source is built by a build script or a test
python3 "$SCRIPT_DIR/test_source_orphans.py"
echo "[-] Running OpenAPI Specification Test Suite..."
python3 "$SCRIPT_DIR/test_openapi.py" --host 127.0.0.1 --port 9999
python3 "$SCRIPT_DIR/test_mqtt_ha_template.py"

# 7. Device shell scripts run on the host (paths moved to a temp dir)
python3 "$SCRIPT_DIR/test_hub_user_sh.py"

echo "==========================================================="
echo "  ALL MOCKING & AUTOMATED TESTS COMPLETED WITH 100% PASS   "
echo "==========================================================="
