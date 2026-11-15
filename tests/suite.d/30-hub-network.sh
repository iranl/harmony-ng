# Hub network services and Hub shell scripts (network, update, maintenance)

# Wi-Fi values written as typed, checked before use, one Wi-Fi test at a time
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_wpa_conf" "$SCRIPT_DIR/test_wpa_conf.c" \
    "$REPO_ROOT/payload/source/hub_network_util.c" "$REPO_ROOT/payload/source/cJSON.c" -ldl -lm && "$BUILD_DIR/test_wpa_conf"
# Wi-Fi and static Ethernet settings the Hub cannot use get 400 (daemon on a private mock root, port 9143)
