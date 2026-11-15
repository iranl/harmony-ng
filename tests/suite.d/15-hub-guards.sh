# hub_daemon guards that refuse or hold a change while it would break something (sourced by
# tests/run_mock_suite.sh). Daemon tests use ports 9200-9219 and their own mock roots.

echo "[-] Hub guards: no update reboot during an activity switch..."
python3 "$SCRIPT_DIR/test_update_activity_guard.py"
python3 "$SCRIPT_DIR/test_update_reboot_waits.py"
echo "[-] Hub guards: accounts and tokens made while authentication was off are listed..."
python3 "$SCRIPT_DIR/test_auth_switch_on_list.py"
python3 "$SCRIPT_DIR/test_hub_user_marks.py"
echo "[-] Hub guards: a device still in use is not deleted..."
python3 "$SCRIPT_DIR/test_device_delete_in_use.py"
