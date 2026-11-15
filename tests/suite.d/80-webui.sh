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
