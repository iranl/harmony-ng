# hub_daemon_test on the mock root (3 of 3): MQTT, RF key channel, IR hold, Logitech shim, web UI checks
# (moved from tests/run_mock_suite.sh; the suite sources this file)

# bt_reconnect.sh in a host sandbox (a stale pid file never kills another process)
python3 "$SCRIPT_DIR/test_bt_reconnect_pidfile.py"
python3 "$SCRIPT_DIR/test_mqtt_v2.py"
python3 "$SCRIPT_DIR/test_rf_key_channel.py"
# A held infrared key repeats until its release (real sends through the mock transmitter, timed)
python3 "$SCRIPT_DIR/test_ir_hold.py"
# The held-key player sleeps while idle also where futex waits never sleep (the Hub's kernel)
python3 "$SCRIPT_DIR/test_ir_hold_idle_cpu.py"
# New web UI Wi-Fi form against a stubbed fetch (Node, no Hub)
if command -v node >/dev/null 2>&1; then node "$SCRIPT_DIR/test_frontend_wifi_save.mjs"; else echo "    (node not found, test_frontend_wifi_save.mjs skipped)"; fi
python3 "$SCRIPT_DIR/test_cmd_actions.py"
python3 "$SCRIPT_DIR/test_rf_status_push.py"
python3 "$SCRIPT_DIR/test_boot_time.py"
python3 "$SCRIPT_DIR/test_hbus.py"
python3 "$SCRIPT_DIR/test_ssdp.py"
# New web UI dialogs with a failing Hub call (Node, stand-in DOM, no Hub)
if command -v node >/dev/null 2>&1; then node "$SCRIPT_DIR/test_frontend_modal_errors.mjs"; else echo "    (node not found, test_frontend_modal_errors.mjs skipped)"; fi
python3 "$SCRIPT_DIR/test_hbus_aioharmony.py"
python3 "$SCRIPT_DIR/test_save_json_types.py"
python3 "$SCRIPT_DIR/test_icon_proxy_limits.py"
python3 "$SCRIPT_DIR/test_device_delay_limits.py"

# Web UI log viewers offer only sources GET /api/logs serves
python3 "$SCRIPT_DIR/test_webui_log_sources.py"
