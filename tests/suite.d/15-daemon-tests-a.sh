# hub_daemon_test on the mock root (1 of 3): WebSocket events, status, HTTP worker timeouts
# (moved from tests/run_mock_suite.sh; the suite sources this file)

# 5b. WebSocket event bus tests against hub_daemon_test on the mock root
python3 "$SCRIPT_DIR/test_poweroff_activity.py"
echo "[-] Running WebSocket event tests..."
python3 "$SCRIPT_DIR/test_activity_repeat_request.py"
python3 "$SCRIPT_DIR/test_ws_events.py"
python3 "$SCRIPT_DIR/test_reentrant_libc.py"
python3 "$SCRIPT_DIR/test_status.py"
python3 "$SCRIPT_DIR/test_http_worker_timeouts.py"
