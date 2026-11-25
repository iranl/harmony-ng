# Host tools: decisions of #220 (migration ids and repeats, SSH host keys)

# tools/migrate_hub_configs.py: stock IR commands get no "repeats", devices keep PressMinRepeats
python3 "$SCRIPT_DIR/test_migrate_stock_repeats.py"
# tools/migrate_hub_configs.py: stock transports, power, inputs, maps and PressMinRepeats
python3 "$SCRIPT_DIR/test_migrate_stock_config.py"
# tools/migrate_hub_configs.py: long device ids get short ones, every reference follows
python3 "$SCRIPT_DIR/test_migrate_device_ids.py"
# tools/migrate_hub_configs.py: stock devices, power, inputs and remote maps from roles and ButtonMaps
python3 "$SCRIPT_DIR/test_migrate_stock_config.py"
# tools/migrate_hub_configs.py: two remotes of one Hub keep their own maps, a Companion its activity keys
python3 "$SCRIPT_DIR/test_migrate_companion_maps.py"
# Host tools check SSH host keys: accept-new, project known_hosts, --reset-host-key (fake ssh)
python3 "$SCRIPT_DIR/test_ssh_host_keys.py"
# tools/clean_stock_resources.py: empties /data/resources only after the stock import, never globs ".."
python3 "$SCRIPT_DIR/test_clean_stock_resources.py"
