# Bluetooth: the pinned BLE remote and a plugin's adv_listen, and broadcasts from another address
# (addr=): the real bt_listen.c and bt_adv.c against the fake BTstack, with a clock of its own
build_test -Wall -Wextra -Wno-format-truncation -O1 -g -fsanitize=address,undefined -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/hub_btstack" \
    -o "$BUILD_DIR/test_bt_listen_remote" "$SCRIPT_DIR/test_bt_listen_remote.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_status_json.c" \
    "$REPO_ROOT/payload/source/hub_btstack/bt_le_pair_window.c" "$REPO_ROOT/payload/source/cJSON.c" -lm \
    && "$BUILD_DIR/test_bt_listen_remote"
# the test plays bt_main.c and bt_remote.c: every place there that makes the remote ready tells
# bt_listen.c (the next line calls bt_listen_remote_ready())
awk '/app_state = APP_READY;/ { want = FILENAME ":" FNR; next }
     want { if ($0 !~ /bt_listen_remote_ready\(\);/) { print "[-] FAIL: " want ": APP_READY without bt_listen_remote_ready()"; bad = 1 } else n++; want = "" }
     END { if (bad || n < 3) { if (!bad) print "[-] FAIL: " n " places call bt_listen_remote_ready(), 3 expected"; exit 1 }
           print "[+] every APP_READY in hub_btstack calls bt_listen_remote_ready() (" n " places)" }' \
    "$REPO_ROOT"/payload/source/hub_btstack/*.c
