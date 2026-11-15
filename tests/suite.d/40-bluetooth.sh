# Bluetooth (hub_btstack and the hub_daemon side of it)
# hub_btstack's command lines, host table, kept wake key, releases and files, against a fake BTstack
build_test -Wall -Wextra -Wno-format-truncation -O1 -g -fsanitize=address,undefined -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/hub_btstack" \
    -o "$BUILD_DIR/test_bt_hub_btstack" "$SCRIPT_DIR/test_bt_hub_btstack.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_status_json.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_le_pair_window.c" "$REPO_ROOT/payload/source/cJSON.c" -lm \
    && "$BUILD_DIR/test_bt_hub_btstack"
# hub_daemon side: addresses and pipe fields checked, release_all, button event slot, Bluetooth off
python3 "$SCRIPT_DIR/test_bt_api_checks.py"
