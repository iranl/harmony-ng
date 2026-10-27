#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_PORT=8998
HOST_BUILD_DIR="$REPO_ROOT/build/tmp/test_host"
mkdir -p "$HOST_BUILD_DIR"

echo "[-] Compiling host test runner for auth test..."
gcc -Wall -O2 -g -DTEST_HARNESS=1 \
    -I"$REPO_ROOT/payload/source" \
    "$REPO_ROOT/payload/source/hub_daemon.c" \
    "$REPO_ROOT/payload/source/http_server.c" \
    "$REPO_ROOT/payload/source/http_util.c" \
    "$REPO_ROOT/payload/source/http_auth.c" \
    "$REPO_ROOT/payload/source/http_api_auth.c" \
    "$REPO_ROOT/payload/source/http_api_activities.c" \
    "$REPO_ROOT/payload/source/http_api_devices.c" \
    "$REPO_ROOT/payload/source/http_api_commands.c" \
    "$REPO_ROOT/payload/source/http_api_custom_icons.c" \
    "$REPO_ROOT/payload/source/http_api_network.c" \
    "$REPO_ROOT/payload/source/hub_network_util.c" \
    "$REPO_ROOT/payload/source/http_api_remotes.c" \
    "$REPO_ROOT/payload/source/http_api_ota.c" \
    "$REPO_ROOT/payload/source/http_api_backup.c" \
    "$REPO_ROOT/payload/source/http_api_system.c" \
    "$REPO_ROOT/payload/source/config_store.c" \
    "$REPO_ROOT/payload/source/config_sanitize.c" \
    "$REPO_ROOT/payload/source/config_migration.c" \
    "$REPO_ROOT/payload/source/orchestrator.c" \
    "$REPO_ROOT/payload/source/orchestrator_queue.c" \
    "$REPO_ROOT/payload/source/hub_mqtt.c" \
    "$REPO_ROOT/payload/source/hub_mqtt_ha.c" \
    "$REPO_ROOT/payload/source/hub_ws.c" \
    "$REPO_ROOT/payload/source/hub_ntp.c" \
    "$REPO_ROOT/payload/source/resource_cache.c" \
    "$REPO_ROOT/payload/source/cJSON.c" \
    "$REPO_ROOT/payload/source/ir_encoder.c" \
    "$REPO_ROOT/payload/source/ir_i2s.c" \
    "$REPO_ROOT/payload/source/hw_action.c" \
    "$REPO_ROOT/payload/source/ws_server.c" \
    "$REPO_ROOT/payload/source/mqtt_client.c" \
    "$REPO_ROOT/payload/source/sha256.c" \
    -lpthread -lm \
    -o "$HOST_BUILD_DIR/hub_daemon_test"

# Prepare test directories and test user
mkdir -p /data/harmony/config 2>/dev/null || mkdir -p /tmp/data_harmony/config
if [ -d "/data/harmony" ] && [ -w "/data/harmony" ]; then
    HARMONY_CFG="/data/harmony/config"
else
    mkdir -p /data 2>/dev/null || true
    if [ ! -d "/data/harmony" ]; then
        ln -s /tmp/data_harmony /data/harmony 2>/dev/null || true
    fi
    HARMONY_CFG="/data/harmony/config"
fi

# Create test users: admin (pass: secret123), guest (pass: guestpass)
cat << 'EOF' > "$HARMONY_CFG/users.json"
{
  "auth_enabled": true,
  "users": [
    {
      "username": "admin",
      "password": "secret123",
      "role": "admin"
    },
    {
      "username": "guest",
      "password": "guestpass",
      "role": "user"
    }
  ]
}
EOF
rm -f "$HARMONY_CFG/tokens.json" 2>/dev/null || true

# Launch daemon
echo "[-] Launching test daemon on port $TEST_PORT..."
"$HOST_BUILD_DIR/hub_daemon_test" --dry-run --port "$TEST_PORT" >/tmp/hub_auth_daemon.log 2>&1 &
DAEMON_PID=$!

cleanup() {
    echo "[-] Stopping daemon (PID $DAEMON_PID)..."
    kill -9 "$DAEMON_PID" 2>/dev/null || true
    rm -f "$HARMONY_CFG/users.json" "$HARMONY_CFG/tokens.json" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for daemon
for i in $(seq 1 20); do
    if curl -s "http://127.0.0.1:$TEST_PORT/health" > /dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

echo "[+] Daemon is live! Testing authentication..."

# Test 1: Unauthenticated request to /api/status -> 401 with Digest & Basic headers
echo "[-] Test 1: 401 Challenge headers on protected endpoint..."
HEADERS=$(curl -s -i "http://127.0.0.1:$TEST_PORT/api/status" | tr -d '\r')
echo "$HEADERS" | grep -i "401 Unauthorized" > /dev/null
echo "$HEADERS" | grep -i 'WWW-Authenticate: Digest realm="Harmony Hub"' > /dev/null
echo "$HEADERS" | grep -i 'WWW-Authenticate: Basic realm="Harmony Hub"' > /dev/null
echo "    PASS: 401 response contains Digest and Basic challenges"

# Test 2: HTTP Basic auth backward compatibility
echo "[-] Test 2: Basic auth fallback..."
RESP=$(curl -s -u admin:secret123 "http://127.0.0.1:$TEST_PORT/api/auth/me")
echo "$RESP" | grep '"ok":true' > /dev/null
echo "$RESP" | grep '"username":"admin"' > /dev/null
echo "    PASS: Basic auth works"

# Test 3: HTTP Digest authentication (RFC 7616 SHA-256)
echo "[-] Test 3: HTTP Digest auth via curl..."
DIGEST_RESP=$(curl -s --digest -u admin:secret123 "http://127.0.0.1:$TEST_PORT/api/auth/me")
echo "$DIGEST_RESP" | grep '"ok":true' > /dev/null
echo "$DIGEST_RESP" | grep '"username":"admin"' > /dev/null
echo "    PASS: HTTP Digest auth succeeded"

# Test 4: OAuth2 POST /api/auth/token login
echo "[-] Test 4: OAuth2 Bearer token login (POST /api/auth/token)..."
LOGIN_RESP=$(curl -s -X POST -H "Content-Type: application/json" \
    -d '{"username":"admin","password":"secret123"}' \
    "http://127.0.0.1:$TEST_PORT/api/auth/token")
echo "$LOGIN_RESP" | grep '"ok":true' > /dev/null
TOKEN=$(echo "$LOGIN_RESP" | grep -o '"access_token":"[^"]*"' | cut -d'"' -f4)
if [ -z "$TOKEN" ] || [[ ! "$TOKEN" == hht_* ]]; then
    echo "FAIL: Invalid token format: $TOKEN"
    exit 1
fi
echo "    PASS: Acquired Bearer token: ${TOKEN:0:16}..."

# Test 5: Authenticate using Bearer token
echo "[-] Test 5: Authenticate using Bearer token..."
ME_RESP=$(curl -s -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$TEST_PORT/api/auth/me")
echo "$ME_RESP" | grep '"ok":true' > /dev/null
echo "$ME_RESP" | grep '"username":"admin"' > /dev/null
echo "    PASS: Bearer token authentication verified"

# Test 6: Create Personal Access Token (PAT)
echo "[-] Test 6: Generate Personal Access Token (PAT)..."
PAT_RESP=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d '{"name":"Home Assistant","role":"admin"}' \
    "http://127.0.0.1:$TEST_PORT/api/auth/tokens")
echo "$PAT_RESP" | grep '"ok":true' > /dev/null
PAT_TOKEN=$(echo "$PAT_RESP" | grep -o '"token":"[^"]*"' | cut -d'"' -f4)
PAT_ID=$(echo "$PAT_RESP" | grep -o '"id":"[^"]*"' | cut -d'"' -f4)
if [ -z "$PAT_TOKEN" ] || [[ ! "$PAT_TOKEN" == pat_* ]]; then
    echo "FAIL: Invalid PAT token format: $PAT_TOKEN"
    exit 1
fi
echo "    PASS: Created PAT $PAT_ID (${PAT_TOKEN:0:12}...)"

# Test 7: Use PAT token to access protected endpoint
echo "[-] Test 7: Request with PAT Bearer token..."
PAT_AUTH_RESP=$(curl -s -H "Authorization: Bearer $PAT_TOKEN" "http://127.0.0.1:$TEST_PORT/api/auth/me")
echo "$PAT_AUTH_RESP" | grep '"ok":true' > /dev/null
echo "$PAT_AUTH_RESP" | grep '"username":"pat:Home Assistant"' > /dev/null
echo "    PASS: PAT authentication verified"

# Test 8: Revoke PAT token
echo "[-] Test 8: Revoke PAT token..."
REVOKE_RESP=$(curl -s -X DELETE -H "Authorization: Bearer $TOKEN" \
    "http://127.0.0.1:$TEST_PORT/api/auth/tokens?id=$PAT_ID")
echo "$REVOKE_RESP" | grep '"ok":true' > /dev/null

# Test 9: Verify revoked PAT is rejected
echo "[-] Test 9: Verifying revoked PAT is rejected..."
REVOKED_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $PAT_TOKEN" "http://127.0.0.1:$TEST_PORT/api/auth/me")
if [ "$REVOKED_STATUS" != "401" ]; then
    echo "FAIL: Revoked PAT returned HTTP $REVOKED_STATUS, expected 401"
    exit 1
fi
echo "    PASS: Revoked PAT correctly rejected with 401"

# Test 10: Session Logout
echo "[-] Test 10: Session logout (POST /api/auth/logout)..."
LOGOUT_RESP=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$TEST_PORT/api/auth/logout")
echo "$LOGOUT_RESP" | grep '"ok":true' > /dev/null
LOGOUT_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$TEST_PORT/api/auth/me")
if [ "$LOGOUT_STATUS" != "401" ]; then
    echo "FAIL: Logged out token returned HTTP $LOGOUT_STATUS, expected 401"
    exit 1
fi
echo "    PASS: Session token successfully invalidated"

echo "=============================================="
echo "  ALL AUTHENTICATION TESTS PASSED (10/10)!    "
echo "=============================================="
