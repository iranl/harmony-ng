# Tests, CI and leftovers (sourced by tests/run_mock_suite.sh; bash, set -e, build_test,
# SCRIPT_DIR, REPO_ROOT, BUILD_DIR from there)

# Remote UI mock test with array bounds checks: a test that writes past a fixed array (more
# devices than g_rf_devices holds) stops here instead of passing by luck of the memory layout
build_test -Wall -O1 -g -fsanitize=bounds -fno-sanitize-recover=bounds \
    -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" \
    "$SCRIPT_DIR/test_remote_mock_ui.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c \
    -lpthread -lm -o "$BUILD_DIR/test_remote_mock_ui_bounds"
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_mock_ui_bounds"

# /api/mqtt-publish needs the same role as /api/button-press; the snapshot hub object has unique keys
python3 "$SCRIPT_DIR/test_route_alias_role.py"

# .gitignore ignores the local work folders at the top of the checkout only
python3 "$SCRIPT_DIR/test_gitignore_rules.py"

# Test harness and runners: a busy port or a binary built with other flags is not reused;
# tests/run_tests.sh stops when the host daemon does not compile
python3 "$SCRIPT_DIR/test_harness_guards.py"
python3 "$SCRIPT_DIR/test_run_tests_script.py"
