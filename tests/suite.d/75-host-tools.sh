# Host tools: decisions of #220 (migration ids and repeats, SSH host keys)

# tools/migrate_hub_configs.py: stock IR commands get no "repeats" (the Hub's default applies)
python3 "$SCRIPT_DIR/test_migrate_stock_repeats.py"
