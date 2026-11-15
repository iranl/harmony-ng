# Bluetooth: device links handed to hub_daemon, and forgetting a host
# hub_btstack keeps a device link hub_daemon did not answer and sends it again (it wrote
# devices.json itself), without blocking its run loop; against a fake BTstack and a fake hub_daemon
# on loopback port 9241
build_test -Wall -Wextra -Wno-format-truncation -O1 -g -fsanitize=address,undefined -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/hub_btstack" \
    -o "$BUILD_DIR/test_bt_link_retry" "$SCRIPT_DIR/test_bt_link_retry.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_status_json.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_le_pair_window.c" "$REPO_ROOT/payload/source/cJSON.c" -lm \
    && "$BUILD_DIR/test_bt_link_retry"
