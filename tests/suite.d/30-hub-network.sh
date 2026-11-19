# Hub network services and Hub shell scripts (network, update, maintenance)

# Wi-Fi values written as typed, checked before use, one Wi-Fi test at a time (wifi_config.c)
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_wpa_conf" "$SCRIPT_DIR/test_wpa_conf.c" \
    "$REPO_ROOT/payload/source/hub_network_util.c" "$REPO_ROOT/payload/source/wifi_config.c" "$REPO_ROOT/payload/source/cJSON.c" -ldl -lm && "$BUILD_DIR/test_wpa_conf"
# Wi-Fi and static Ethernet settings the Hub cannot use get 400 (daemon on a private mock root, port 9143)
python3 "$SCRIPT_DIR/test_network_settings_api.py"
# network_manager.sh: Ethernet settings read, DNS list, wpa_supplicant configuration file
python3 "$SCRIPT_DIR/test_network_manager_sh.py"
# mDNS and SSDP answer unicast queries only from senders on the Hub's own links
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_net_source_local" "$SCRIPT_DIR/test_net_source_local.c" \
    "$REPO_ROOT/payload/source/hub_network_util.c" "$REPO_ROOT/payload/source/wifi_config.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_net_source_local"
# update.sh: the token is one header argument, one apply at a time (sandbox, stub busybox)
python3 "$SCRIPT_DIR/test_update_sh.py"
# update-apply refused while one runs (409), update-check writes only on change (port 9145)
python3 "$SCRIPT_DIR/test_update_api_once.py"
# factory_reset.sh: supervisors stopped, hub_sync and read-only remount before the reset
python3 "$SCRIPT_DIR/test_factory_reset_sh.py"
# init.sh: profile files, dropbear wrapper and links rewritten only when they change
python3 "$SCRIPT_DIR/test_init_flash_writes.py"
# start_usb_eth.sh: the EHCI device is registered at most once per boot
python3 "$SCRIPT_DIR/test_start_usb_eth_once.py"
# hub_util mknod refuses a bad type, eth_fix reports failed offload changes
python3 "$SCRIPT_DIR/test_hub_util_cli.py"
