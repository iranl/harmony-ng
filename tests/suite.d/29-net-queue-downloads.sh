# HTTP/TCP devices: one lookup per send, connection to the checked address (product build, no
# packets leave), the full send queue; update.sh downloads that fail TLS, its lock, the init.sh
# repair loop log
build_test -Wall -Wno-format-truncation -O1 -g -fsanitize=address -pthread -DNET_NO_TLS -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_net_resolve_once" "$SCRIPT_DIR/test_net_resolve_once.c" \
    "$REPO_ROOT"/payload/source/{tls_client,http_client,cJSON}.c -lm && "$BUILD_DIR/test_net_resolve_once"
python3 "$SCRIPT_DIR/test_net_queue_full.py"
python3 "$SCRIPT_DIR/test_update_downloads.py"
