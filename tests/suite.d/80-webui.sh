# Web UIs (legacy android-app/ and new frontend/): Node tests with a stand-in DOM and a stubbed
# fetch, no browser and no Hub. Skipped when node is not installed.
webui_node_test() {
    if command -v node >/dev/null 2>&1; then
        node "$SCRIPT_DIR/$1"
    else
        echo "    (node not found, $1 skipped)"
    fi
}
echo "[-] Running web UI tests..."
webui_node_test test_frontend_auth.mjs
webui_node_test test_frontend_connection.mjs
webui_node_test test_webui_keymap_load.mjs
webui_node_test test_webui_command_port.mjs
# RF remote page: auto-map wiring and aliases, no device scope for a remote without a screen
webui_node_test test_webui_remote_slots.mjs
webui_node_test test_frontend_b25.mjs
webui_node_test test_webui_dead_routes.mjs
# Every Hub path the two UIs call has a handler in payload/source
python3 "$SCRIPT_DIR/test_webui_routes.py"
webui_node_test test_webui_form_limits.mjs
# Renderer import versions and Android backup flags of both UIs (static checks)
python3 "$SCRIPT_DIR/test_webui_packaging.py"
webui_node_test test_webui_i18n.mjs
# The Wi-Fi setup over USB switch keeps the Ethernet settings in both UIs
webui_node_test test_webui_usb_wifi_toggle.mjs
# Legacy UI follows a Hub update to its real end: failure, restart, version
webui_node_test test_webui_hub_update_state.mjs
# The update banner button starts the Hub update
webui_node_test test_webui_update_banner.mjs
# Plugin devices: boolean config fields are checkboxes, default commands keep irKeyCode / irWhen
webui_node_test test_webui_plugin_fields.mjs
