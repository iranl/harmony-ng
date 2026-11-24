# Legacy web UI: the device power dialog shows and saves its own device in every step row
# (Node, stand-in DOM, no browser and no Hub; skipped without node)
if command -v node >/dev/null 2>&1; then
    node "$SCRIPT_DIR/test_webui_power_seq_device.mjs"
else
    echo "    (node not found, test_webui_power_seq_device.mjs skipped)"
fi
