# Host tools: install, deploy, backup, recovery, migration and build scripts

# tools/migrate_hub_configs.py: odd input shapes, failed runs, secret file modes, long device ids
python3 "$SCRIPT_DIR/test_migrate_input_shapes.py"
# tools/backup_hub.py and build/deploy_hub.py: Hub check, required backup, per-Hub rotation, bundle VERSION
python3 "$SCRIPT_DIR/test_hub_tools_target.py"
# remote/deploy_remote.py and remote/setup_remote.py: --no-screen on every path, ARM binaries, ssh config
python3 "$SCRIPT_DIR/test_remote_tools_options.py"
# remote/bootstrap_serial.py against a fake serial console (stock shell without base64)
python3 "$SCRIPT_DIR/test_bootstrap_serial.py"
# tools/usb_state.py picks the remote's network adapter by its USB ids (not a phone or a dongle)
python3 "$SCRIPT_DIR/test_usb_state_adapter.py"
# tools/unbrick_recovery/serve_dropbear.py serves only the Hub (loopback, port 9220)
python3 "$SCRIPT_DIR/test_serve_dropbear.py"
# build/build_btstack.sh applies btstack.patch to a clean clone (toy git repository)
sh "$SCRIPT_DIR/test_build_btstack_patch.sh"
# scripts/bump_version.py: missing files are errors, lockfile root version, --next for the release
python3 "$SCRIPT_DIR/test_bump_version.py"
# Build downloads checked by SHA-256 (fake curl), theme download cache keyed by URL
sh "$SCRIPT_DIR/test_build_downloads.sh"
python3 "$SCRIPT_DIR/test_build_theme_cache.py"
# Committed binaries match the SHA-256 list in tools/bin/README.md
sh "$SCRIPT_DIR/test_prebuilt_hashes.sh"
# build/build_webui.py, build_apk.py, deploy_hub.py: one web UI packaging step, results checked
python3 "$SCRIPT_DIR/test_webui_sync.py"
# tools/remote_render: --select kept inside the activity list (UBSan), unreadable input files
sh "$SCRIPT_DIR/test_remote_render_select.sh"
