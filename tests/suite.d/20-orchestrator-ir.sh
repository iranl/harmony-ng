# Activity orchestrator, command dispatch and IR code handling (hub_daemon in --dry-run on its
# own mock root; ports 9125 and 9129)
echo "[-] Running test_orchestrator_tail.py..."
python3 "$SCRIPT_DIR/test_orchestrator_tail.py"
echo "[-] Running test_dispatch_commands.py..."
python3 "$SCRIPT_DIR/test_dispatch_commands.py"
