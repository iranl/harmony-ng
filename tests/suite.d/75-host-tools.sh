# Host tools: decisions of #220 (migration ids and repeats, SSH host keys)

# tools/migrate_hub_configs.py: stock IR commands get no "repeats" (the Hub's default applies)
python3 "$SCRIPT_DIR/test_migrate_stock_repeats.py"
# tools/migrate_hub_configs.py: long device ids get short ones, every reference follows
python3 "$SCRIPT_DIR/test_migrate_device_ids.py"
# Host tools check SSH host keys: accept-new, project known_hosts, --reset-host-key (fake ssh)
python3 "$SCRIPT_DIR/test_ssh_host_keys.py"
