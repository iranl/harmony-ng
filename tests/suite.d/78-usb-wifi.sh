# Wi-Fi setup over USB (hub_util usbwifi): the applet answers the stock usbhid's frames for
# /sys/wifi/connect only, tests the network first and keeps the old settings on failure
# (hub_util for the host, fake wpa_cli, tools/harmony_hid.py as the PC side, port 9174)
python3 "$SCRIPT_DIR/test_usb_wifi.py"
