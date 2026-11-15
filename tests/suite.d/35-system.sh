# Hub system: SSH logins, network time, factory reset

# Every path that starts SSH on the Hub starts dropbear for key logins only (-s -g)
python3 "$SCRIPT_DIR/test_hub_ssh_keys_only.py"
