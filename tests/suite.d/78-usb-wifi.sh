# Wi-Fi setup over USB (hub_util usbwifi): the applet answers the stock usbhid's frames for
# /sys/wifi/connect only, tests the network first and keeps the old settings on failure
# (hub_util for the host, fake wpa_cli, tools/harmony_hid.py as the PC side, port 9174)
python3 "$SCRIPT_DIR/test_usb_wifi.py"
# Its shell parts in sandboxes: start_usb_eth.sh (host mode written first, driver unload waited
# for), the init.sh supervisor and stop, the rcS.local emergency start, the USB_WIFI_SETUP switch
python3 "$SCRIPT_DIR/test_usb_wifi_scripts.py"
# hub_daemon side of the shared Wi-Fi test: a cut-short test undone at start, a failed web UI
# test on a Hub with no Wi-Fi file leaves none (port 9175)
python3 "$SCRIPT_DIR/test_wifi_test_recover.py"
