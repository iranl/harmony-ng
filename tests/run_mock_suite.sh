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

# 4. Run Core Component Unit Tests
echo "[-] Running core component unit tests..."
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_activity_icons" "$SCRIPT_DIR/test_activity_icons.c" && "$BUILD_DIR/test_activity_icons"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_remote_i18n" "$SCRIPT_DIR/test_remote_i18n.c" "$REPO_ROOT/payload/source/remote/remote_i18n.c" && "$BUILD_DIR/test_remote_i18n"
sh "$SCRIPT_DIR/test_remote_migrate.sh"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_migrate" "$SCRIPT_DIR/test_remote_migrate.c" "$REPO_ROOT/payload/source/remote/remote_migrate.c" && "$BUILD_DIR/test_remote_migrate"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_battery" "$SCRIPT_DIR/test_remote_battery.c" "$REPO_ROOT/payload/source/remote/remote_battery.c" && "$BUILD_DIR/test_remote_battery"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_flightlog" "$SCRIPT_DIR/test_remote_flightlog.c" "$REPO_ROOT/payload/source/remote/remote_flightlog.c" && "$BUILD_DIR/test_remote_flightlog"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_custom_icon_registry" "$SCRIPT_DIR/test_custom_icon_registry.c" "$REPO_ROOT/payload/source/custom_icon_registry.c" "$REPO_ROOT/payload/source/cJSON.c" "$REPO_ROOT/payload/source/sha256.c" -lm && LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_custom_icon_registry"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sanitize_activity_steps" "$SCRIPT_DIR/test_sanitize_activity_steps.c" "$REPO_ROOT/payload/source/config_sanitize.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_sanitize_activity_steps"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sha256" "$SCRIPT_DIR/test_sha256.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_sha256"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mqtt_connect_flags" "$SCRIPT_DIR/test_mqtt_connect_flags.c" "$REPO_ROOT/payload/source/mqtt_client.c" && "$BUILD_DIR/test_mqtt_connect_flags"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_encoder" "$SCRIPT_DIR/test_ir_encoder.c" "$REPO_ROOT/payload/source/ir_encoder.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_encoder"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_button_list" "$SCRIPT_DIR/test_rf_button_list.c" "$REPO_ROOT/payload/source/rf_button_list.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_rf_button_list"
build_test -Wall -O2 -fsanitize=address -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_keymap_lock" "$SCRIPT_DIR/test_rf_keymap_lock.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_rf_keymap_lock" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_keymap_lock"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_capture" "$SCRIPT_DIR/test_ir_capture.c" "$REPO_ROOT/payload/source/ir_encoder.c" "$REPO_ROOT/payload/source/ir_i2s.c" -lpthread && "$BUILD_DIR/test_ir_capture" "$SCRIPT_DIR/fixtures/ir/samsung36_volume_up.i2s"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_action_lifecycle" "$SCRIPT_DIR/test_action_lifecycle.c" "$REPO_ROOT/payload/source/hw_action.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_action_lifecycle"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_bt_state_migrate" "$SCRIPT_DIR/test_bt_state_migrate.c" "$REPO_ROOT/payload/source/bt_migrate.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_bt_state_migrate"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mfg" "$SCRIPT_DIR/test_mfg.c" "$REPO_ROOT/payload/source/mfg_id.c" && "$BUILD_DIR/test_mfg"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ws_read_frame" "$SCRIPT_DIR/test_ws_read_frame.c" "$REPO_ROOT/payload/source/ws_server.c" -lpthread && "$BUILD_DIR/test_ws_read_frame"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mdns_packet" "$SCRIPT_DIR/test_mdns_packet.c" "$REPO_ROOT/payload/source/hub_mdns.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_mdns_packet"
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mqtt_client_io" "$SCRIPT_DIR/test_mqtt_client_io.c" "$REPO_ROOT/payload/source/mqtt_client.c" && "$BUILD_DIR/test_mqtt_client_io"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_hbus_config" "$SCRIPT_DIR/test_hbus_config.c" "$REPO_ROOT/payload/source/hbus_config.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_hbus_config" "$SCRIPT_DIR/fixtures/ha/sample_config"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_hub_events" "$SCRIPT_DIR/test_hub_events.c" "$REPO_ROOT/payload/source/hub_events.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_hub_events"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_status_json" "$SCRIPT_DIR/test_rf_status_json.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_status_json"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_model" "$SCRIPT_DIR/test_rf_model.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_model"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_battery" "$SCRIPT_DIR/test_rf_battery.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_battery"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_rf_ota_resume" "$SCRIPT_DIR/test_rf_ota_resume.c" "$REPO_ROOT/payload/source/remote/remote_rf_ota.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_rf_ota_resume"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_migrate" "$SCRIPT_DIR/test_rf_migrate.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_migrate"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_ota_lifecycle" "$SCRIPT_DIR/test_rf_ota_lifecycle.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_sync.c" "$REPO_ROOT/payload/source/rf_ota.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_ota_lifecycle"

# 4b. IR receiver: decoding a real recording, and a send while a stalled receiver is closing
gcc -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_rx_decode" "$SCRIPT_DIR/test_ir_rx_decode.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_rx_decode" "$SCRIPT_DIR/fixtures/ir/samsung36_volume_up.i2s"
gcc -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_tx_stall" "$SCRIPT_DIR/test_ir_tx_stall.c" "$REPO_ROOT/payload/source/ir_i2s.c" && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_ir_tx_stall"

# 4c. hub_btstack reconnect queue and connected-hosts file
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_reconnect_queue" "$SCRIPT_DIR/test_bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" && "$BUILD_DIR/test_bt_reconnect_queue"

# 5. Run Hub & Remote Full Co-Simulation
echo "[-] Launching Full Hub & Remote Co-Simulation..."
python3 "$SCRIPT_DIR/test_co_simulation.py"

# 5a. Bluetooth links around activity power on and off (orchestrator, daemon in --dry-run)
python3 "$SCRIPT_DIR/test_bt_power_link.py"
python3 "$SCRIPT_DIR/test_activity_queue.py"
python3 "$SCRIPT_DIR/test_activity_switch_timing.py"

# 5b. WebSocket event bus tests against hub_daemon_test on the mock root
echo "[-] Running WebSocket event tests..."
python3 "$SCRIPT_DIR/test_ws_events.py"
python3 "$SCRIPT_DIR/test_status.py"
python3 "$SCRIPT_DIR/test_ws_auth.py"
python3 "$SCRIPT_DIR/test_ws_keepalive.py"
python3 "$SCRIPT_DIR/test_ir_raw.py"
python3 "$SCRIPT_DIR/test_mdns.py"
python3 "$SCRIPT_DIR/test_no_lan_responders.py"
python3 "$SCRIPT_DIR/test_mqtt_v2.py"
python3 "$SCRIPT_DIR/test_cmd_actions.py"
python3 "$SCRIPT_DIR/test_rf_status_push.py"
python3 "$SCRIPT_DIR/test_boot_time.py"
python3 "$SCRIPT_DIR/test_hbus.py"
python3 "$SCRIPT_DIR/test_ssdp.py"
python3 "$SCRIPT_DIR/test_hbus_aioharmony.py"

# 6. Run OpenAPI Specification Contract Validation (docs/openapi.yaml must parse as YAML)
echo "[-] Validating docs/openapi.yaml with PyYAML..."
python3 -c "import yaml, sys; yaml.safe_load(open('$REPO_ROOT/docs/openapi.yaml')); print('    openapi.yaml parses')"
echo "[-] Running OpenAPI Specification Test Suite..."
python3 "$SCRIPT_DIR/test_openapi.py" --host 127.0.0.1 --port 9999

echo "==========================================================="
echo "  ALL MOCKING & AUTOMATED TESTS COMPLETED WITH 100% PASS   "
echo "==========================================================="
