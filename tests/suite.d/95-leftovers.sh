# Leftovers removed: unused route aliases answer 404, their canonical routes still work

# hub_daemon: GET /api/wifi/scan and POST /api/auth/tokens/delete are gone (auth on, port 9340)
python3 "$SCRIPT_DIR/test_route_aliases_removed.py"
