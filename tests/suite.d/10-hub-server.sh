# hub_daemon HTTP server, routes, ids, config sanitizer and thread safety (sourced by
# tests/run_mock_suite.sh). Daemon tests use ports 9100-9119 and their own mock roots.

echo "[-] Hub server: HTTP replies and request parsing..."
python3 "$SCRIPT_DIR/test_http_server_replies.py"
build_test -Wall -O2 -Wno-format-truncation -fsanitize=signed-integer-overflow -fno-sanitize-recover=all \
    -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ipv4_digits" "$SCRIPT_DIR/test_ipv4_digits.c" \
    "$REPO_ROOT/payload/source/http_util.c" "$REPO_ROOT/payload/source/cJSON.c" && "$BUILD_DIR/test_ipv4_digits"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_api_body" "$SCRIPT_DIR/test_rf_api_body.c" \
    "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" \
    -lpthread -lm && "$BUILD_DIR/test_rf_api_body"
echo "[-] Hub server: route checks (map saves, custom icons, query parameters, logs)..."
python3 "$SCRIPT_DIR/test_hub_route_checks.py"
# backup bundle with every section (six slot maps, slot table, keys) under AddressSanitizer
python3 "$SCRIPT_DIR/test_backup_bundle_all.py"
build_test -Wall -O2 -fsanitize=float-cast-overflow -fno-sanitize-recover=all -I"$REPO_ROOT/payload/source" \
    -o "$BUILD_DIR/test_config_values" "$SCRIPT_DIR/test_config_values.c" "$REPO_ROOT/payload/source/config_sanitize.c" \
    "$REPO_ROOT/payload/source/timezone_db.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_config_values"
echo "[-] Hub server: threads (activity requests at once, shim requests, settings across threads)..."
python3 "$SCRIPT_DIR/test_activity_double_start.py"
python3 "$SCRIPT_DIR/test_hbus_pending.py"
python3 "$SCRIPT_DIR/test_settings_threads_tsan.py"
