# HTTP/TCP devices: one lookup per send, connection to the checked address (product build, no
# packets leave), the full send queue, live sends (a person's press dropped when late); update.sh
# downloads that fail TLS, its lock, the init.sh repair loop log
build_test -Wall -Wno-format-truncation -O1 -g -fsanitize=address -pthread -DNET_NO_TLS -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_net_resolve_once" "$SCRIPT_DIR/test_net_resolve_once.c" \
    "$REPO_ROOT"/payload/source/{tls_client,http_client,cJSON}.c -lm && "$BUILD_DIR/test_net_resolve_once"
python3 "$SCRIPT_DIR/test_net_queue_full.py"
python3 "$SCRIPT_DIR/test_net_live_sends.py"
# the BLE remote's device commands go to hub_daemon as live sends
build_test -Wall -Wextra -Wno-format-truncation -O1 -g -fsanitize=address,undefined -pthread -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/hub_btstack" \
    -o "$BUILD_DIR/test_bt_remote_live" "$SCRIPT_DIR/test_bt_remote_live.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_status_json.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_le_pair_window.c" "$REPO_ROOT/payload/source/cJSON.c" -lm \
    && "$BUILD_DIR/test_bt_remote_live"
python3 "$SCRIPT_DIR/test_update_downloads.py"
