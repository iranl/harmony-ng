# Bluetooth: device links handed to hub_daemon, and forgetting a host
# hub_btstack keeps a device link hub_daemon did not answer and sends it again (it wrote
# devices.json itself), without blocking its run loop; against a fake BTstack and a fake hub_daemon
# on loopback port 9241
build_test -Wall -Wextra -Wno-format-truncation -O1 -g -fsanitize=address,undefined -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/hub_btstack" \
    -o "$BUILD_DIR/test_bt_link_retry" "$SCRIPT_DIR/test_bt_link_retry.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_status_json.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_le_pair_window.c" "$REPO_ROOT/payload/source/cJSON.c" -lm \
    && "$BUILD_DIR/test_bt_link_retry"
# host_forget in hub_btstack: link key, link, host table, reconnects (also while a connection is
# being set up), kept key, waiting link, default host file
build_test -Wall -Wextra -Wno-format-truncation -O1 -g -fsanitize=address,undefined -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/hub_btstack" \
    -o "$BUILD_DIR/test_bt_forget_host_cmd" "$SCRIPT_DIR/test_bt_forget_host_cmd.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_status_json.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_le_pair_window.c" "$REPO_ROOT/payload/source/cJSON.c" -lm \
    && "$BUILD_DIR/test_bt_forget_host_cmd"
# POST /api/bt-forget-host: address check, Bluetooth off, devices.json, default host file, admin only;
# 500 when hub_btstack is gone; POST /api/bt-link-device 404/400 (hub_daemon on ports 9242, 9243
# and 9245, own mock roots). It starts a process named hub_btstack for a few seconds, which another
# test running at the same time would see: run the suite under /tmp/hng_suite.lock as usual (not
# taken here: the suite may hold it).
python3 "$SCRIPT_DIR/test_bt_forget_host.py"
# the Forget button's API call and texts in both web UIs
if command -v node >/dev/null 2>&1; then node "$SCRIPT_DIR/test_frontend_bt_forget.mjs"; else echo "    (node not found, test_frontend_bt_forget.mjs skipped)"; fi
# how hub_daemon hands commands to hub_btstack: socket only for host_forget (-1 when nobody listens),
# socket or command file for the others
build_test -Wall -Wextra -O1 -g -fsanitize=address,undefined -I"$REPO_ROOT/payload/source" \
    -o "$BUILD_DIR/test_bt_cmd_send" "$SCRIPT_DIR/test_bt_cmd_send.c" && "$BUILD_DIR/test_bt_cmd_send"
