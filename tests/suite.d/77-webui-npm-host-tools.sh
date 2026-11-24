# build/build_webui.py and build/deploy_hub.py run `npm ci` before the web UI build when
# node_modules is missing or was installed from another package-lock.json (fake npm)
python3 "$SCRIPT_DIR/test_webui_npm_install.py"
