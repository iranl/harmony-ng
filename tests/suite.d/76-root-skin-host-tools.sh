# tools/root_hub_usb.py: the skin id the Hub reports in hex ("0x6A") is read, and the firmware check
# asks for that skin before the default one (fake update service, no USB)
python3 "$SCRIPT_DIR/test_root_hub_skin.py"
