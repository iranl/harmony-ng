# Host tools: install, deploy, backup, recovery, migration and build scripts

# tools/migrate_hub_configs.py: odd input shapes, failed runs, secret file modes, long device ids
python3 "$SCRIPT_DIR/test_migrate_input_shapes.py"
# tools/backup_hub.py and build/deploy_hub.py: Hub check, required backup, per-Hub rotation, bundle VERSION
python3 "$SCRIPT_DIR/test_hub_tools_target.py"
# remote/deploy_remote.py and remote/setup_remote.py: --no-screen on every path, ARM binaries, ssh config
python3 "$SCRIPT_DIR/test_remote_tools_options.py"
