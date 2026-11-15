# Host tools: install, deploy, backup, recovery, migration and build scripts

# tools/migrate_hub_configs.py: odd input shapes, failed runs, secret file modes, long device ids
python3 "$SCRIPT_DIR/test_migrate_input_shapes.py"
# tools/backup_hub.py and build/deploy_hub.py: Hub check, required backup, per-Hub rotation, bundle VERSION
python3 "$SCRIPT_DIR/test_hub_tools_target.py"
# remote/deploy_remote.py and remote/setup_remote.py: --no-screen on every path, ARM binaries, ssh config
python3 "$SCRIPT_DIR/test_remote_tools_options.py"
# tools/sync_remote_to_hub.py: import flag, --force, --skip-icons, failed transfers (fake ssh)
python3 "$SCRIPT_DIR/test_sync_remote_to_hub.py"
# remote/bootstrap_serial.py against a fake serial console (stock shell without base64)
python3 "$SCRIPT_DIR/test_bootstrap_serial.py"
