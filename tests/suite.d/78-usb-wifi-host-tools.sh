# tools/harmony_hid.py honours the device's error answer (byte 2 = 0xFF); tools/set_wifi_usb.py
# against a fake Hub (no USB device)
python3 "$SCRIPT_DIR/test_hid_error_marker.py"
python3 "$SCRIPT_DIR/test_set_wifi_usb.py"
