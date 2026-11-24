# hub_daemon_test on the mock root (2 of 3): caching, WebSocket auth and keepalive, SNTP, mDNS, auth
# (moved from tests/run_mock_suite.sh; the suite sources this file)

python3 "$SCRIPT_DIR/test_static_cache_key.py"
python3 "$SCRIPT_DIR/test_ws_auth.py"
python3 "$SCRIPT_DIR/test_ws_slow_frame.py"
python3 "$SCRIPT_DIR/test_lan_request_guards.py"
python3 "$SCRIPT_DIR/test_ws_keepalive.py"
python3 "$SCRIPT_DIR/test_ws_slow_reader.py"
python3 "$SCRIPT_DIR/test_sntp_nonblocking.py"
python3 "$SCRIPT_DIR/test_ir_raw.py"
# #260: a saved command's port and repeats from a form post, used by a send; activities in Display Order
python3 "$SCRIPT_DIR/test_command_port_order.py"
python3 "$SCRIPT_DIR/test_request_body_limit.py"
python3 "$SCRIPT_DIR/test_activity_help_answer.py"
python3 "$SCRIPT_DIR/test_mdns.py"
python3 "$SCRIPT_DIR/test_import_validate.py"
python3 "$SCRIPT_DIR/test_auth_hardening.py"
python3 "$SCRIPT_DIR/test_no_lan_responders.py"
