# Plugins: BLE advertising payload, plugin manager (install checks, manifest validation, lifecycle) and
# device save over the API
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ble_adv_payload" "$SCRIPT_DIR/test_ble_adv_payload.c" && "$BUILD_DIR/test_ble_adv_payload"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -DHARMONY_PLUGIN_ROOT="\"$BUILD_DIR/plugin_root\"" -o "$BUILD_DIR/test_plugin_mgr" "$SCRIPT_DIR/test_plugin_mgr.c" "$REPO_ROOT/payload/source/plugin_mgr.c" "$REPO_ROOT/payload/source/http_util.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_plugin_mgr"
python3 "$SCRIPT_DIR/test_plugin_device_save.py"
