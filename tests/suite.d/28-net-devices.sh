# HTTP(S) and TCP devices, and hub_fetch (BearSSL: hub_fetch.c, tls_client.c, http_client.c,
# net_action.c). Both build a host libbearssl.a first (tests/build_bearssl_host.sh).
python3 "$SCRIPT_DIR/test_tls_fetch.py"
python3 "$SCRIPT_DIR/test_net_device_command.py"
