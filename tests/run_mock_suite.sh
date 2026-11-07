#!/bin/bash
set -euo pipefail

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
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_activity_icons" "$SCRIPT_DIR/test_activity_icons.c" && "$BUILD_DIR/test_activity_icons"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_remote_i18n" "$SCRIPT_DIR/test_remote_i18n.c" "$REPO_ROOT/payload/source/remote/remote_i18n.c" && "$BUILD_DIR/test_remote_i18n"
sh "$SCRIPT_DIR/test_remote_migrate.sh"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_migrate" "$SCRIPT_DIR/test_remote_migrate.c" "$REPO_ROOT/payload/source/remote/remote_migrate.c" && "$BUILD_DIR/test_remote_migrate"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_custom_icon_registry" "$SCRIPT_DIR/test_custom_icon_registry.c" "$REPO_ROOT/payload/source/custom_icon_registry.c" "$REPO_ROOT/payload/source/cJSON.c" "$REPO_ROOT/payload/source/sha256.c" -lm && LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_custom_icon_registry"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sanitize_activity_steps" "$SCRIPT_DIR/test_sanitize_activity_steps.c" "$REPO_ROOT/payload/source/config_sanitize.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_sanitize_activity_steps"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sha256" "$SCRIPT_DIR/test_sha256.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_sha256"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_encoder" "$SCRIPT_DIR/test_ir_encoder.c" "$REPO_ROOT/payload/source/ir_encoder.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_encoder"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_action_lifecycle" "$SCRIPT_DIR/test_action_lifecycle.c" "$REPO_ROOT/payload/source/hw_action.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_action_lifecycle"
gcc -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_bt_state_migrate" "$SCRIPT_DIR/test_bt_state_migrate.c" "$REPO_ROOT/payload/source/bt_migrate.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_bt_state_migrate"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_rf_ota_resume" "$SCRIPT_DIR/test_rf_ota_resume.c" "$REPO_ROOT/payload/source/remote/remote_rf_ota.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_rf_ota_resume"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_migrate" "$SCRIPT_DIR/test_rf_migrate.c" "$REPO_ROOT/payload/source/rf_state.c" && "$BUILD_DIR/test_rf_migrate"

# 5. Run Hub & Remote Full Co-Simulation
echo "[-] Launching Full Hub & Remote Co-Simulation..."
python3 "$SCRIPT_DIR/test_co_simulation.py"

# 6. Run OpenAPI Specification Contract Validation
echo "[-] Running OpenAPI Specification Test Suite..."
python3 "$SCRIPT_DIR/test_openapi.py" --host 127.0.0.1 --port 9999

echo "==========================================================="
echo "  ALL MOCKING & AUTOMATED TESTS COMPLETED WITH 100% PASS   "
echo "==========================================================="
