# Plugins: BLE advertising payload, plugin manager (install checks, manifest validation, lifecycle) and
# device save over the API, IR alternative of a plugin command
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ble_adv_payload" "$SCRIPT_DIR/test_ble_adv_payload.c" && "$BUILD_DIR/test_ble_adv_payload"
# plugin sockets in a short folder of this checkout (a socket path has 108 bytes); a 3 s
# SIGTERM grace keeps the test short
PLUGIN_TEST_RUN="/tmp/hhp-$(printf '%s' "$BUILD_DIR" | sha1sum | cut -c1-8)-t"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -DHARMONY_PLUGIN_ROOT="\"$BUILD_DIR/plugin_root\"" -DHARMONY_PLUGINS_RUN_DIR="\"$PLUGIN_TEST_RUN\"" -DPLUGIN_TERM_GRACE_S=3 -DPLUGIN_TMP_STATFS_PATH="\"/proc\"" -DPLUGIN_MEMINFO_PATH="\"$BUILD_DIR/plugin_meminfo\"" -o "$BUILD_DIR/test_plugin_mgr" "$SCRIPT_DIR/test_plugin_mgr.c" "$REPO_ROOT/payload/source/plugin_mgr.c" "$REPO_ROOT/payload/source/http_util.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lm -lpthread && "$BUILD_DIR/test_plugin_mgr"
# a plugin command with an infrared alternative (irKeyCode, irWhen) goes out as IR while the
# device's config switch is on
python3 "$SCRIPT_DIR/test_plugin_ir_commands.py"
python3 "$SCRIPT_DIR/test_plugin_device_save.py"
python3 "$SCRIPT_DIR/test_plugin_pair_listen.py"
python3 "$SCRIPT_DIR/test_plugin_hub_name.py"
