# tools/harmony_hid.py on Linux without a hidraw node: the usbfs transport, its errors and the
# message the tools print (issue #241; fake sysfs and ioctl, no USB device)
python3 "$SCRIPT_DIR/test_hid_usbfs.py"
