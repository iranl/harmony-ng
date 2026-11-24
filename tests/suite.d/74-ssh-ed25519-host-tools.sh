# Host tools: the Ed25519 key checks that decide whether the Hub gets the Ed25519-only SSH server

# build/deploy_hub.py and payload/scripts/update.sh: same patterns, same answer for every
# authorized_keys line; tools/root_hub_usb.py picks the Ed25519 harmony_owner key in both modes
python3 "$SCRIPT_DIR/test_ssh_ed25519_checks.py"
