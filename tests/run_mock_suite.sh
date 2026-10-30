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

# 3. Run Headless Elite UI Engine Mock Test
echo "[-] Compiling & running test_elite_mock_ui..."
gcc -Wall -O2 \
    -I"$REPO_ROOT/payload/source" \
    -I"$REPO_ROOT/payload/source/elite" \
    "$SCRIPT_DIR/test_elite_mock_ui.c" \
    "$REPO_ROOT/payload/source/elite/elite_config.c" \
    "$REPO_ROOT/payload/source/elite/elite_fb.c" \
    "$REPO_ROOT/payload/source/elite/elite_i18n.c" \
    "$REPO_ROOT/payload/source/elite/elite_input.c" \
    "$REPO_ROOT/payload/source/elite/elite_power.c" \
    "$REPO_ROOT/payload/source/elite/elite_rf.c" \
    "$REPO_ROOT/payload/source/elite/elite_rf_ota.c" \
    "$REPO_ROOT/payload/source/elite/elite_rf_sync.c" \
    "$REPO_ROOT/payload/source/elite/elite_state.c" \
    "$REPO_ROOT/payload/source/elite/elite_ui.c" \
    "$REPO_ROOT/payload/source/elite/elite_ui_activities.c" \
    "$REPO_ROOT/payload/source/elite/elite_ui_devices.c" \
    "$REPO_ROOT/payload/source/elite/elite_ui_pairing.c" \
    "$REPO_ROOT/payload/source/elite/elite_ui_settings.c" \
    "$REPO_ROOT/payload/source/elite/elite_ui_settings_actions.c" \
    "$REPO_ROOT/payload/source/elite_stock_icons.c" \
    "$REPO_ROOT/payload/source/cJSON.c" \
    "$REPO_ROOT/payload/source/sha256.c" \
    -lpthread -lm \
    -o "$BUILD_DIR/test_elite_mock_ui"

HARMONY_MOCK_DIR="/tmp/harmony_mock" \
LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" \
    "$BUILD_DIR/test_elite_mock_ui"

# 4. Run Core Component Unit Tests
echo "[-] Running core component unit tests..."
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_activity_icons" "$SCRIPT_DIR/test_activity_icons.c" && "$BUILD_DIR/test_activity_icons"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_elite_i18n" "$SCRIPT_DIR/test_elite_i18n.c" "$REPO_ROOT/payload/source/elite/elite_i18n.c" && "$BUILD_DIR/test_elite_i18n"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_custom_icon_registry" "$SCRIPT_DIR/test_custom_icon_registry.c" "$REPO_ROOT/payload/source/custom_icon_registry.c" "$REPO_ROOT/payload/source/cJSON.c" "$REPO_ROOT/payload/source/sha256.c" -lm && "$BUILD_DIR/test_custom_icon_registry"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sanitize_activity_steps" "$SCRIPT_DIR/test_sanitize_activity_steps.c" "$REPO_ROOT/payload/source/config_sanitize.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_sanitize_activity_steps"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_sha256" "$SCRIPT_DIR/test_sha256.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_sha256"
gcc -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_encoder" "$SCRIPT_DIR/test_ir_encoder.c" "$REPO_ROOT/payload/source/ir_encoder.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_encoder"

# 5. Run Hub & Remote Full Co-Simulation
echo "[-] Launching Full Hub & Remote Co-Simulation..."
python3 "$SCRIPT_DIR/test_co_simulation.py"

echo "==========================================================="
echo "  ALL MOCKING & AUTOMATED TESTS COMPLETED WITH 100% PASS   "
echo "==========================================================="
