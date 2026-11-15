#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_PORT="${TEST_PORT:-8999}"

echo "=============================================="
echo "  Harmony Hub Comprehensive Test Suite        "
echo "  (Dry-Run & API Validation)                  "
echo "=============================================="

# Build (or reuse when nothing changed) the host daemon the same way the Python harness does:
# one source list (tests/hub_harness.py), rebuilt when a source or the compile command changed.
# A failed build stops the script: the API tests below need this binary.
HOST_BUILD_DIR="$REPO_ROOT/build/tmp/test_host"
mkdir -p "$HOST_BUILD_DIR"
echo "[-] Building host test daemon (tests/hub_harness.py)..."
if ! python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import hub_harness; hub_harness.build_hub_daemon_test(); hub_harness.build_mock_lib()' "$SCRIPT_DIR"; then
    echo "[-] FAIL: the host test daemon did not compile"
    exit 1
fi

run_api_tests() {
    local bin="$1"
    echo "[-] Launching test daemon in DRY-RUN mode on port $TEST_PORT..."
    if curl -s -o /dev/null "http://127.0.0.1:$TEST_PORT/health"; then
        echo "[-] FAIL: port $TEST_PORT is already in use by another process"
        exit 1
    fi
    # Private mock root: /data and /cache of the daemon live in a temp dir, not on the host
    MOCK_ROOT=$(mktemp -d)
    mkdir -p "$MOCK_ROOT/data/harmony/config"

    # Launch daemon with dry-run flag
    HARMONY_MOCK_DIR="$MOCK_ROOT" HARMONY_MOCK_ROLE=hub LD_PRELOAD="$HOST_BUILD_DIR/libmock_hardware.so" \
        "$bin" --dry-run --port "$TEST_PORT" >"$MOCK_ROOT/daemon.log" 2>&1 &
    DAEMON_PID=$!

    cleanup() {
        echo "[-] Stopping test daemon (PID $DAEMON_PID)..."
        kill -9 "$DAEMON_PID" 2>/dev/null || true
        rm -rf "$MOCK_ROOT"
    }
    trap cleanup EXIT

    # Wait for daemon to listen; give up when it exits or never answers
    echo "[-] Waiting for daemon to initialize..."
    local live=0
    for i in $(seq 1 25); do
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            break
        fi
        if curl -s "http://127.0.0.1:$TEST_PORT/health" >/dev/null 2>&1; then
            live=1
            break
        fi
        sleep 0.2
    done
    if [ "$live" != 1 ]; then
        echo "[-] FAIL: the test daemon did not answer on port $TEST_PORT"
        cat "$MOCK_ROOT/daemon.log" || true
        exit 1
    fi

    echo "[+] Daemon is live! Running test cases..."

    # Test 1: Health Check
    echo -n "Test 1: GET /health ... "
    HEALTH_RESP=$(curl -s "http://127.0.0.1:$TEST_PORT/health")
    if echo "$HEALTH_RESP" | grep -q '"status":"ok"'; then
        echo "PASS"
    else
        echo "FAIL: $HEALTH_RESP"
        exit 1
    fi

    # Test 2: GET /api/settings
    echo -n "Test 2: GET /api/settings (Dry-Run status) ... "
    SETTINGS_RESP=$(curl -s "http://127.0.0.1:$TEST_PORT/api/settings")
    if echo "$SETTINGS_RESP" | grep -q '"dry_run":true'; then
        echo "PASS"
    else
        echo "FAIL: $SETTINGS_RESP"
        exit 1
    fi

    # Test 3: POST /api/settings (Update Hub settings)
    echo -n "Test 3: POST /api/settings (Update Hub settings) ... "
    POST_SETTINGS_RESP=$(curl -s -X POST "http://127.0.0.1:$TEST_PORT/api/settings" \
        -H "Content-Type: application/json" \
        -d '{"hub_name":"Living Room Hub","dry_run":1}')
    if echo "$POST_SETTINGS_RESP" | grep -q '"hub_name":"Living Room Hub"'; then
        echo "PASS"
    else
        echo "FAIL: $POST_SETTINGS_RESP"
        exit 1
    fi

    # Test 4: POST /api/ir-send (Direct Pronto Hex Blast in Dry-Run)
    echo -n "Test 4: POST /api/ir-send (Direct Pronto Hex Blast) ... "
    # Sony Power On Pronto Hex test signal
    PRONTO_HEX="0000 0067 0000 000d 0060 0018 0030 0018 0018 0018 0030 0018 0018 0018 0030 0018 0018 0018 0018 0018 0030 0018 0018 0018 0018 0018 0018 0018 0018 03a0"
    IR_RESP=$(curl -s -X POST "http://127.0.0.1:$TEST_PORT/api/ir-send" \
        -H "Content-Type: application/json" \
        -d "{\"pronto\":\"$PRONTO_HEX\",\"port\":7,\"repeats\":1}")
    if echo "$IR_RESP" | grep -q '"ok":true'; then
        echo "PASS"
    else
        echo "FAIL: $IR_RESP"
        exit 1
    fi

    # Test 5: POST /api/activity-stop in Dry-Run
    echo -n "Test 5: POST /api/activity-stop (Dry-run activity stop) ... "
    ACT_RESP=$(curl -s -X POST "http://127.0.0.1:$TEST_PORT/api/activity-stop" \
        -H "Content-Type: application/json" \
        -d '{"dry_run":1}')
    if echo "$ACT_RESP" | grep -q -E '"target":|"success":'; then
        echo "PASS"
    else
        echo "FAIL: $ACT_RESP"
        exit 1
    fi

    # Test 6: GET /api/wifi-scan
    echo -n "Test 6: GET /api/wifi-scan (Wi-Fi scanning) ... "
    SCAN_RESP=$(curl -s "http://127.0.0.1:$TEST_PORT/api/wifi-scan")
    if echo "$SCAN_RESP" | grep -q '"ok":true' && echo "$SCAN_RESP" | grep -q '"networks":'; then
        echo "PASS"
    else
        echo "FAIL: $SCAN_RESP"
        exit 1
    fi

    # Test 7: POST /api/wifi-test (Empty SSID validation)
    echo -n "Test 7: POST /api/wifi-test (Validation on empty SSID) ... "
    BAD_TEST_RESP=$(curl -s -w "\n%{http_code}" -X POST "http://127.0.0.1:$TEST_PORT/api/wifi-test" \
        -H "Content-Type: application/json" \
        -d '{"ssid":""}')
    HTTP_CODE=$(echo "$BAD_TEST_RESP" | tail -n1)
    if [ "$HTTP_CODE" -eq 400 ]; then
        echo "PASS"
    else
        echo "FAIL (HTTP $HTTP_CODE): $BAD_TEST_RESP"
        exit 1
    fi

    # Test 8: POST /api/wifi-test (Valid test connection in test harness)
    echo -n "Test 8: POST /api/wifi-test (Valid test connection) ... "
    GOOD_TEST_RESP=$(curl -s -X POST "http://127.0.0.1:$TEST_PORT/api/wifi-test" \
        -H "Content-Type: application/json" \
        -d '{"ssid":"TestMeshNetwork","password":"SecretPassword123"}')
    if echo "$GOOD_TEST_RESP" | grep -q '"ok":true'; then
        echo "PASS"
    else
        echo "FAIL: $GOOD_TEST_RESP"
        exit 1
    fi

    # Test 9: POST /api/wifi-save (Save Wi-Fi settings without force apply)
    echo -n "Test 9: POST /api/wifi-save (Safe Wi-Fi config save) ... "
    SAVE_RESP=$(curl -s -X POST "http://127.0.0.1:$TEST_PORT/api/wifi-save" \
        -H "Content-Type: application/json" \
        -d '{"ssid":"NewHomeWiFi","password":"NewPassword456","apply":"save"}')
    if echo "$SAVE_RESP" | grep -q '"ok":true'; then
        echo "PASS"
    else
        echo "FAIL: $SAVE_RESP"
        exit 1
    fi

    echo "=============================================="
    echo "  ALL TESTS PASSED SUCCESSFULLY!              "
    echo "=============================================="
}

if command -v gcc >/dev/null 2>&1; then
    echo "[-] Running RF loopback binding verification..."
    gcc -Wall -O2 -I"$REPO_ROOT/payload/source" \
        -o "$HOST_BUILD_DIR/test_rf_loopback" \
        "$SCRIPT_DIR/test_rf_loopback.c" \
        "$REPO_ROOT/payload/source/rf_state.c" \
        "$REPO_ROOT/payload/source/rf_api.c" \
        "$REPO_ROOT/payload/source/cJSON.c" \
        -lpthread -lm
    "$HOST_BUILD_DIR/test_rf_loopback"
fi

run_api_tests "$HOST_BUILD_DIR/hub_daemon_test"
echo "[-] Running WebSocket event tests..."
python3 "$SCRIPT_DIR/test_ws_events.py"
python3 "$SCRIPT_DIR/test_status.py"
python3 "$SCRIPT_DIR/test_ws_auth.py"
python3 "$SCRIPT_DIR/test_ws_keepalive.py"
python3 "$SCRIPT_DIR/test_ir_raw.py"
python3 "$SCRIPT_DIR/test_mdns.py"
python3 "$SCRIPT_DIR/test_mqtt_v2.py"
