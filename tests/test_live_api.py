#!/usr/bin/env python3
"""
Harmony Hub & Codex External API Test Suite
Validates all HTTP REST endpoints and WebSocket protocols on a live or emulated Hub.
Supports dry-run verification, schema validation, and timing analysis.
"""

import argparse
import json
import sys
import time
import urllib.request
import urllib.error
import socket

# ANSI Colors
C_GREEN = "\033[92m"
C_RED = "\033[91m"
C_YELLOW = "\033[93m"
C_CYAN = "\033[96m"
C_BOLD = "\033[1m"
C_RESET = "\033[0m"

class ApiTester:
    def __init__(self, host="192.168.1.1", port=80, timeout=4.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.base_url = f"http://{host}:{port}"
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def http_request(self, path, method="GET", data=None, headers=None):
        url = f"{self.base_url}{path}"
        req_headers = {"User-Agent": "CodexExternalApiTester/1.0", "Accept": "application/json"}
        if headers:
            req_headers.update(headers)
        
        body = None
        if data is not None:
            if isinstance(data, dict):
                body = json.dumps(data).encode("utf-8")
                req_headers["Content-Type"] = "application/json"
            elif isinstance(data, str):
                body = data.encode("utf-8")
        
        req = urllib.request.Request(url, data=body, headers=req_headers, method=method)
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                elapsed_ms = (time.time() - t0) * 1000.0
                raw_text = resp.read().decode("utf-8", errors="replace")
                parsed_json = None
                try:
                    parsed_json = json.loads(raw_text)
                except Exception:
                    pass
                return resp.status, parsed_json, raw_text, elapsed_ms
        except urllib.error.HTTPError as e:
            elapsed_ms = (time.time() - t0) * 1000.0
            raw_text = e.read().decode("utf-8", errors="replace")
            parsed_json = None
            try:
                parsed_json = json.loads(raw_text)
            except Exception:
                pass
            return e.code, parsed_json, raw_text, elapsed_ms
        except Exception as e:
            elapsed_ms = (time.time() - t0) * 1000.0
            return 0, None, str(e), elapsed_ms

    def assert_test(self, test_name, success, detail=""):
        if success:
            self.passed += 1
            print(f"  [{C_GREEN}PASS{C_RESET}] {test_name} {C_CYAN}({detail}){C_RESET}")
        else:
            self.failed += 1
            print(f"  [{C_RED}FAIL{C_RESET}] {test_name} - {C_RED}{detail}{C_RESET}")

    def test_ws_handshake(self, ws_port):
        """Simple TCP raw WebSocket client handshake test."""
        test_name = f"WebSocket Handshake (ws://{self.host}:{ws_port}/)"
        t0 = time.time()
        try:
            s = socket.create_connection((self.host, ws_port), timeout=self.timeout)
            key = "dGhlIHNhbXBsZSBub25jZQ=="
            req = (
                f"GET / HTTP/1.1\r\n"
                f"Host: {self.host}:{ws_port}\r\n"
                f"Upgrade: websocket\r\n"
                f"Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                f"Sec-WebSocket-Version: 13\r\n\r\n"
            )
            s.sendall(req.encode("ascii"))
            resp = s.recv(1024).decode("utf-8", errors="replace")
            s.close()
            elapsed_ms = (time.time() - t0) * 1000.0
            is_101 = "101 Switching Protocols" in resp
            self.assert_test(test_name, is_101, f"{elapsed_ms:.1f}ms - 101 Upgrade Verified" if is_101 else resp.splitlines()[0] if resp else "Empty response")
        except Exception as e:
            self.assert_test(test_name, False, f"Connection failed: {e}")

    def run_all(self):
        print(f"\n{C_BOLD}=== Testing Harmony Codex API at {self.base_url} ==={C_RESET}\n")

        # 1. Health
        status, j, text, ms = self.http_request("/health")
        self.assert_test("GET /health", status == 200 and (j and j.get("status") == "ok"), f"{ms:.1f}ms")

        # 2. Settings (Verify Dry-Run & BT supervision schema)
        status, j, text, ms = self.http_request("/api/settings")
        schema_ok = status == 200 and j and "bt_auto_reconnect" in j and "bt_reconnect_duration_sec" in j
        self.assert_test("GET /api/settings", schema_ok, f"{ms:.1f}ms, dry_run={j.get('dry_run') if j else 'N/A'}")

        # 3. Save Settings
        status, j, text, ms = self.http_request("/api/settings", method="POST", data={
            "bt_auto_reconnect": 1,
            "bt_reconnect_duration_sec": 45,
            "bt_reconnect_delay_sec": 5
        })
        post_ok = status == 200 and j and j.get("ok") is True and j.get("bt_reconnect_duration_sec") == 45
        self.assert_test("POST /api/settings (Update BT Reconnect)", post_ok, f"{ms:.1f}ms")

        # 4. Inventory
        status, j, text, ms = self.http_request("/api/inventory")
        inv_ok = status == 200 and j and ("devices" in j or "DevicesWithFeatures" in j or isinstance(j, list))
        dev_count = len(j.get("devices", [])) if (j and isinstance(j, dict) and "devices" in j) else 0
        self.assert_test("GET /api/inventory", inv_ok, f"{ms:.1f}ms, {dev_count} devices found")

        # 5. Activities
        status, j, text, ms = self.http_request("/api/activities")
        act_ok = status == 200 and j and ("activities" in j or isinstance(j, list))
        self.assert_test("GET /api/activities", act_ok, f"{ms:.1f}ms, currentId={j.get('currentId') if isinstance(j, dict) else 'N/A'}")

        # 6. IR Send Direct Pronto Test
        # Sony Power On Pronto Hex
        sony_power_pronto = "0000 0067 0000 000d 0060 0018 0030 0018 0018 0018 0030 0018 0018 0018 0030 0018 0018 0018 0018 0018 0030 0018 0018 0018 0018 0018 0018 0018 0018 03a0"
        status, j, text, ms = self.http_request("/api/ir-send", method="POST", data={
            "pronto": sony_power_pronto,
            "port": 7,
            "repeats": 1
        })
        self.assert_test("POST /api/ir-send (Direct Pronto Hex)", status == 200 and j and j.get("ok") is True, f"{ms:.1f}ms")

        # 7. Activity Test Sequence Preview (Dry-Run mode)
        status, j, text, ms = self.http_request("/api/activity-stop", method="POST", data={"dry_run": 1})
        self.assert_test("POST /api/activity-stop (Dry-run preview)", status == 200 and j and ("target" in j or "success" in j or "ok" in j), f"{ms:.1f}ms")

        # 8. Bluetooth Status
        status, j, text, ms = self.http_request("/api/bt-status")
        bt_ok = status in (200, 404, 503)
        self.assert_test("GET /api/bt-status", bt_ok, f"{ms:.1f}ms, status={status}")

        # 9. RF Remote Status
        status, j, text, ms = self.http_request("/api/rf/status")
        rf_ok = status in (200, 404, 503)
        self.assert_test("GET /api/rf/status", rf_ok, f"{ms:.1f}ms, status={status}")

        # 10. WebUI Index HTML
        status, j, text, ms = self.http_request("/")
        html_ok = status == 200 and ("<html" in text.lower() or "codex" in text.lower())
        self.assert_test("GET / (WebUI Root)", html_ok, f"{ms:.1f}ms, {len(text)} bytes")

        # 11. WebSocket Live Handshake
        self.test_ws_handshake(self.port)

        # Summary
        total = self.passed + self.failed
        print(f"\n{C_BOLD}=== Summary ==={C_RESET}")
        print(f"Total:  {total}")
        print(f"Passed: {C_GREEN}{self.passed}{C_RESET}")
        print(f"Failed: {C_RED}{self.failed}{C_RESET}\n")

        return 0 if self.failed == 0 else 1

def main():
    parser = argparse.ArgumentParser(description="External API Test Suite for Logitech Harmony Codex Hub")
    parser.add_argument("--host", default="192.168.1.1", help="Target Hub IP address (default: 192.168.1.1)")
    parser.add_argument("--port", type=int, default=80, help="Target Hub HTTP/WS port (default: 80 or 8089)")
    parser.add_argument("--timeout", type=float, default=4.0, help="Socket timeout seconds (default: 4.0)")
    args = parser.parse_args()

    tester = ApiTester(host=args.host, port=args.port, timeout=args.timeout)
    sys.exit(tester.run_all())

if __name__ == "__main__":
    main()
