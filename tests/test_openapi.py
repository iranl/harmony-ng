#!/usr/bin/env python3
"""
Harmony Hub OpenAPI-Driven Test Suite
Validates the OpenAPI 3.1.0 specification integrity and executes functional
tests against a live or emulated Hub driven directly by docs/openapi.yaml.

Requirements: Python 3.8+ (Zero external dependencies, pure stdlib)
Usage:
  python tests/test_openapi.py
  python tests/test_openapi.py --host 192.168.1.1 --port 8089
  python tests/test_openapi.py --spec docs/openapi.yaml --strict
  python -m unittest tests/test_openapi.py
"""

import os
import sys
import re
import json
import socket
import time
import unittest
import urllib.request
import urllib.error

# ANSI terminal colors
C_GREEN = "\033[92m"
C_RED = "\033[91m"
C_YELLOW = "\033[93m"
C_CYAN = "\033[96m"
C_BOLD = "\033[1m"
C_RESET = "\033[0m"

# Default target settings
DEFAULT_SPEC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "openapi.yaml")
DEFAULT_HOST = "192.168.1.1"
DEFAULT_PORT = 8089
DEFAULT_TIMEOUT = 3.5

def parse_yaml_spec(filepath):
    """
    Lightweight, robust YAML parser for OpenAPI 3.1 in pure Python stdlib.
    Extracts paths, operations, parameters, request bodies, and responses.
    """
    if not os.path.isfile(filepath):
        raise FileNotFoundError(f"OpenAPI spec not found at: {filepath}")

    with open(filepath, "r", encoding="utf-8") as f:
        lines = f.readlines()

    root = {}
    stack = [(root, -1)]

    i = 0
    n = len(lines)
    while i < n:
        line = lines[i].rstrip("\r\n")
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            i += 1
            continue

        indent = len(line) - len(line.lstrip(" "))
        while len(stack) > 1 and stack[-1][1] >= indent:
            stack.pop()

        parent = stack[-1][0]

        if stripped.startswith("- "):
            item_str = stripped[2:].strip()
            if not isinstance(parent, list):
                i += 1
                continue
            if ":" in item_str and not item_str.startswith("{"):
                k, v = item_str.split(":", 1)
                new_dict = {}
                parent.append(new_dict)
                stack.append((new_dict, indent))
                if v.strip():
                    new_dict[k.strip().strip("'\"")] = parse_scalar(v.strip())
                else:
                    sub_dict = {}
                    new_dict[k.strip().strip("'\"")] = sub_dict
                    stack.append((sub_dict, indent + 2))
            else:
                if item_str:
                    parent.append(parse_scalar(item_str))
                else:
                    new_obj = {}
                    parent.append(new_obj)
                    stack.append((new_obj, indent))
            i += 1
            continue

        if ":" in stripped:
            key, val = stripped.split(":", 1)
            key = key.strip().strip("'\"")
            val = val.strip()

            if val in ("|", "|-", "|+", ">", ">-", ">+"):
                text_lines = []
                base_indent = None
                i += 1
                while i < n:
                    sub_raw = lines[i].rstrip("\r\n")
                    if not sub_raw:
                        text_lines.append("")
                        i += 1
                        continue
                    sub_indent = len(sub_raw) - len(sub_raw.lstrip(" "))
                    if base_indent is None:
                        if sub_indent <= indent:
                            break
                        base_indent = sub_indent
                    elif sub_indent < base_indent:
                        break
                    text_lines.append(sub_raw[base_indent:])
                    i += 1
                if isinstance(parent, dict):
                    parent[key] = "\n".join(text_lines)
                continue

            if val:
                if isinstance(parent, dict):
                    parent[key] = parse_scalar(val)
            else:
                next_is_list = False
                for j in range(i + 1, min(i + 10, n)):
                    nxt = lines[j].strip()
                    if nxt and not nxt.startswith("#"):
                        if nxt.startswith("- "):
                            next_is_list = True
                        break
                new_container = [] if next_is_list else {}
                if isinstance(parent, dict):
                    parent[key] = new_container
                stack.append((new_container, indent))

        i += 1

    return root

def parse_scalar(val):
    val = val.strip()
    if val.startswith("{") and val.endswith("}"):
        try:
            return json.loads(val)
        except Exception:
            pass
    if val.startswith("[") and val.endswith("]"):
        try:
            return json.loads(val)
        except Exception:
            pass
    if val.lower() == "true": return True
    if val.lower() == "false": return False
    if val.lower() in ("null", "~"): return None
    if (val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'")):
        return val[1:-1]
    try:
        if "." in val:
            return float(val)
        return int(val)
    except ValueError:
        return val

class OpenApiDriver:
    """HTTP client that executes requests and validates against the OpenAPI specification."""
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, timeout=DEFAULT_TIMEOUT):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.base_url = f"http://{host}:{port}"

    def is_reachable(self):
        try:
            req = urllib.request.Request(f"{self.base_url}/health")
            with urllib.request.urlopen(req, timeout=1.5) as resp:
                return resp.status == 200
        except Exception:
            return False

    def request(self, method, path, data=None, query=None, headers=None):
        url = f"{self.base_url}{path}"
        if query:
            q_parts = [f"{k}={v}" for k, v in query.items()]
            url = f"{url}?{'&'.join(q_parts)}"

        req_headers = {
            "User-Agent": "HarmonyOpenApiTester/2.1",
            "Accept": "application/json"
        }
        if headers:
            req_headers.update(headers)

        body = None
        if data is not None:
            if isinstance(data, (dict, list)):
                body = json.dumps(data).encode("utf-8")
                req_headers["Content-Type"] = "application/json"
            elif isinstance(data, str):
                body = data.encode("utf-8")

        req = urllib.request.Request(url, data=body, headers=req_headers, method=method.upper())
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                elapsed_ms = (time.time() - t0) * 1000.0
                raw = resp.read().decode("utf-8", errors="replace")
                parsed = None
                try:
                    parsed = json.loads(raw)
                except Exception:
                    pass
                return resp.status, parsed, raw, elapsed_ms
        except urllib.error.HTTPError as e:
            elapsed_ms = (time.time() - t0) * 1000.0
            raw = e.read().decode("utf-8", errors="replace")
            parsed = None
            try:
                parsed = json.loads(raw)
            except Exception:
                pass
            return e.code, parsed, raw, elapsed_ms
        except Exception as e:
            elapsed_ms = (time.time() - t0) * 1000.0
            return 0, None, str(e), elapsed_ms

    def test_ws_handshake(self, path="/ws"):
        """Verify WebSocket Upgrade on the Hub port."""
        t0 = time.time()
        try:
            s = socket.create_connection((self.host, self.port), timeout=self.timeout)
            key = "dGhlIHNhbXBsZSBub25jZQ=="
            req = (
                f"GET {path} HTTP/1.1\r\n"
                f"Host: {self.host}:{self.port}\r\n"
                f"Upgrade: websocket\r\n"
                f"Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                f"Sec-WebSocket-Version: 13\r\n\r\n"
            )
            s.sendall(req.encode("ascii"))
            resp = s.recv(1024).decode("utf-8", errors="replace")
            s.close()
            elapsed_ms = (time.time() - t0) * 1000.0
            return ("101 Switching Protocols" in resp), elapsed_ms, resp.splitlines()[0] if resp else "Empty"
        except Exception as e:
            return False, 0.0, str(e)


class TestOpenApiCompliance(unittest.TestCase):
    """Test suite combining OpenAPI structural verification with functional Hub validation."""
    
    spec_path = DEFAULT_SPEC
    spec = None
    target_host = DEFAULT_HOST
    target_port = DEFAULT_PORT
    driver = None
    hub_online = False

    @classmethod
    def setUpClass(cls):
        # 1. Parse OpenAPI document
        cls.spec_path = os.environ.get("OPENAPI_SPEC", DEFAULT_SPEC)
        cls.spec = parse_yaml_spec(cls.spec_path)
        
        # 2. Configure target host & port
        cls.target_host = os.environ.get("HUB_HOST", DEFAULT_HOST)
        cls.target_port = int(os.environ.get("HUB_PORT", str(DEFAULT_PORT)))
        cls.driver = OpenApiDriver(host=cls.target_host, port=cls.target_port)
        
        # 3. Check reachability
        cls.hub_online = cls.driver.is_reachable()
        if not cls.hub_online:
            # Fallback probe on localhost
            local_driver = OpenApiDriver(host="127.0.0.1", port=cls.target_port)
            if local_driver.is_reachable():
                cls.driver = local_driver
                cls.target_host = "127.0.0.1"
                cls.hub_online = True

    # =========================================================================
    # CONTRACT & SCHEMA INTEGRITY TESTS (Run offline & in CI)
    # =========================================================================

    def test_openapi_parses_with_pyyaml(self):
        """The file must be valid YAML for real tools, not only for the parser above"""
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed")
        with open(self.spec_path, "r", encoding="utf-8") as f:
            doc = yaml.safe_load(f)
        self.assertIn("paths", doc)
        self.assertIn("/api/ir-raw", doc["paths"])
        self.assertIn("/api/rf/status-changed", doc["paths"])
        self.assertIn("x-websocket-messages", doc["paths"]["/ws"]["get"])

    def test_openapi_contract_metadata(self):
        """OpenAPI root must have valid title, 3.1.x spec version, and matching firmware version."""
        self.assertEqual(self.spec.get("openapi"), "3.1.0", "Specification must be OpenAPI 3.1.0")
        info = self.spec.get("info", {})
        self.assertTrue(info.get("title"), "OpenAPI info.title must be non-empty")
        
        # Version must match payload/source/harmony_version.h
        h_path = os.path.join(os.path.dirname(self.spec_path), "..", "payload", "source", "harmony_version.h")
        if os.path.isfile(h_path):
            with open(h_path, "r", encoding="utf-8") as f:
                content = f.read()
            m = re.search(r'#define\s+HARMONY_VERSION\s+"([^"]+)"', content)
            if m:
                expected_ver = m.group(1)
                self.assertEqual(info.get("version"), expected_ver,
                                 f"OpenAPI info.version ({info.get('version')}) must match HARMONY_VERSION ({expected_ver})")

    def test_openapi_paths_structure(self):
        """All paths in OpenAPI must start with '/' and contain valid HTTP operations."""
        paths = self.spec.get("paths", {})
        self.assertGreater(len(paths), 50, "OpenAPI spec should define all major hub routes")
        for path_name, path_item in paths.items():
            self.assertTrue(path_name.startswith("/"), f"Path {path_name} must begin with '/'")
            if isinstance(path_item, dict):
                for method, op in path_item.items():
                    if method in ("get", "post", "put", "delete", "patch", "options", "head"):
                        self.assertIn("responses", op, f"Operation {method.upper()} {path_name} missing responses block")
                        self.assertIn("summary", op, f"Operation {method.upper()} {path_name} missing summary")

    def test_openapi_coverage_of_core_c_routes(self):
        """Ensure critical firmware endpoints are documented in OpenAPI."""
        paths = self.spec.get("paths", {})
        required_endpoints = [
            "/health",
            "/ws",
            "/api/status",
            "/api/settings",
            "/api/inventory",
            "/api/devices",
            "/api/activities",
            "/api/activity",
            "/api/activity/help",
            "/api/activity/help/answer",
            "/api/poweroff",
            "/api/ir-send",
            "/api/button-press",
            "/api/rf",
            "/api/rf/sleep",
            "/api/remote-status",
            "/api/remote-scan",
            "/api/remote-scan-stop",
            "/api/remote-unpair",
            "/api/update-status",
            "/api/update-check",
            "/api/custom-icons"
        ]
        for ep in required_endpoints:
            self.assertIn(ep, paths, f"Critical endpoint {ep} must be defined in docs/openapi.yaml")

    # =========================================================================
    # FUNCTIONAL API TESTS (Driven by OpenAPI definitions against Hub)
    # =========================================================================

    def require_live_hub(self):
        if not self.hub_online:
            self.skipTest(f"Hub not reachable at http://{self.target_host}:{self.target_port} (Contract verified)")

    def test_live_health_and_websocket(self):
        """Test /health and /ws handshake matching OpenAPI System & Status tag."""
        self.require_live_hub()
        status, j, text, ms = self.driver.request("GET", "/health")
        self.assertEqual(status, 200, f"/health returned HTTP {status}")
        self.assertIsInstance(j, dict)
        self.assertEqual(j.get("status"), "ok")

        # Test WebSocket handshake
        ws_ok, ws_ms, ws_msg = self.driver.test_ws_handshake("/ws")
        self.assertTrue(ws_ok, f"WebSocket /ws handshake failed: {ws_msg} ({ws_ms:.1f}ms)")

    def test_live_system_status_and_telemetry(self):
        """Test /api/status, /api/system-info, and /api/disk-space schemas."""
        self.require_live_hub()
        
        status, j, _, _ = self.driver.request("GET", "/api/status")
        self.assertEqual(status, 200)
        self.assertTrue(j.get("ok"))
        self.assertIn("hub_name", j)
        self.assertIn("state", j)
        self.assertIn("activity", j)
        self.assertIn("mqtt", j)

        status, j, _, _ = self.driver.request("GET", "/api/system-info")
        self.assertEqual(status, 200)
        self.assertIn("uptime", j)
        self.assertIn("kernel", j)

        status, j, _, _ = self.driver.request("GET", "/api/disk-space")
        self.assertEqual(status, 200)
        self.assertTrue(j.get("ok"))
        self.assertIn("total_kb", j)
        self.assertIn("free_kb", j)

    def test_live_settings_crud_and_timezone(self):
        """Test GET and POST /api/settings schema adherence, including timezone support."""
        self.require_live_hub()
        
        status, current_settings, _, _ = self.driver.request("GET", "/api/settings")
        self.assertEqual(status, 200)
        self.assertTrue(current_settings.get("ok"))
        self.assertIn("hub_name", current_settings)
        self.assertIn("timezone", current_settings)
        self.assertIn("led_mode", current_settings)

        orig_dry_run = current_settings.get("dry_run", False)
        # Test POST /api/settings maintaining current name and timezone
        hub_name = current_settings.get("hub_name", "Harmony Hub")
        tz = current_settings.get("timezone", "UTC")
        post_payload = {
            "hub_name": hub_name,
            "timezone": tz,
            "dry_run": True
        }
        status, resp_j, _, _ = self.driver.request("POST", "/api/settings", data=post_payload)
        self.assertEqual(status, 200)
        self.assertTrue(resp_j.get("ok"))
        self.assertEqual(resp_j.get("hub_name"), hub_name)

        # Restore original hub settings so live hub is never left in dry_run mode
        self.driver.request("POST", "/api/settings", data={
            "hub_name": hub_name,
            "timezone": tz,
            "dry_run": orig_dry_run
        })

    def test_live_inventory_and_devices_alias(self):
        """Test /api/inventory and its /api/devices alias schema."""
        self.require_live_hub()
        
        status1, j1, _, _ = self.driver.request("GET", "/api/inventory")
        self.assertEqual(status1, 200)
        self.assertTrue("devices" in j1 or isinstance(j1, list))

        status2, j2, _, _ = self.driver.request("GET", "/api/devices")
        self.assertEqual(status2, 200)
        self.assertEqual(j1, j2, "GET /api/devices should match GET /api/inventory")

    def test_live_activities_and_current_activity_alias(self):
        """Test /api/activities and /api/current_activity schema (busy and sending flags)."""
        self.require_live_hub()
        
        status, act_list, _, _ = self.driver.request("GET", "/api/activities")
        self.assertEqual(status, 200)
        self.assertIn("activities", act_list)
        self.assertIn("currentId", act_list)

        status, cur_act, _, _ = self.driver.request("GET", "/api/activity")
        self.assertEqual(status, 200)
        self.assertIn("activity", cur_act)
        self.assertIn("busy", cur_act)
        self.assertIn("sending", cur_act)

        status_alias, cur_alias, _, _ = self.driver.request("GET", "/api/current_activity")
        self.assertEqual(status_alias, 200)
        self.assertEqual(cur_act, cur_alias, "GET /api/current_activity must match /api/activity")

    def test_live_activity_help_dry_run(self):
        """Test POST /api/activity/help with dry_run=True preview from OpenAPI spec."""
        self.require_live_hub()
        
        # Get active or first activity
        _, cur_act, _, _ = self.driver.request("GET", "/api/activity")
        act_id = cur_act.get("activity") if cur_act else "1"
        if not act_id or act_id == "-1":
            act_id = "1"

        payload = {"activity": str(act_id), "dry_run": True}
        status, resp_j, _, _ = self.driver.request("POST", "/api/activity/help", data=payload)
        
        # Should return 200 OK with preview or 409 if not running
        self.assertIn(status, (200, 409))
        if status == 200:
            self.assertTrue(resp_j.get("success"))
            self.assertIn("toggleDevices", resp_j)
            self.assertIn("preview", resp_j)

    def test_live_hardware_ir_send_schema(self):
        """Test POST /api/ir-send accepts action parameter (tap/press/hold/release)."""
        self.require_live_hub()
        
        # Use Pronto code from OpenAPI example
        sony_power_pronto = "0000 0067 0000 000d 0060 0018 0030 0018 0018 0018 0030 0018 0018 0018 0030 0018 0018 0018 0018 0018 0030 0018 0018 0018 0018 0018 0018 0018 0018 03a0"
        payload = {
            "pronto": sony_power_pronto,
            "port": 7,
            "repeats": 1,
            "action": "tap"
        }
        status, resp_j, _, _ = self.driver.request("POST", "/api/ir-send", data=payload)
        self.assertEqual(status, 200)
        self.assertTrue(resp_j.get("ok"))

    def test_live_rf_status_and_link_telemetry(self):
        """Test GET /api/rf verifies slots and link statistics (frames_rx, retries, etc.)."""
        self.require_live_hub()
        
        status, rf_j, _, _ = self.driver.request("GET", "/api/rf")
        self.assertIn(status, (200, 503))
        if status == 200:
            self.assertTrue(rf_j.get("ok"))
            self.assertIn("slots", rf_j)
            slots = rf_j.get("slots", [])
            self.assertIsInstance(slots, list)
            if len(slots) > 0:
                s1 = slots[0]
                self.assertIn("slot", s1)
                self.assertIn("model", s1)
                self.assertIn("battery", s1)
                if "link" in s1 and isinstance(s1["link"], dict):
                    link = s1["link"]
                    self.assertIn("frames_rx", link)
                    self.assertIn("acked_sent", link)
                    self.assertIn("acked_ok", link)
                    self.assertIn("retries", link)
                    self.assertIn("naks", link)
                    self.assertIn("last_seen_s", link)

    def test_live_ota_update_check_methods(self):
        """Test GET and POST /api/update-check according to OpenAPI specification."""
        self.require_live_hub()
        
        # 1. GET /api/update-check
        status1, j1, _, _ = self.driver.request("GET", "/api/update-check")
        self.assertEqual(status1, 200)
        self.assertTrue(j1.get("ok"))
        self.assertIn("current_version", j1)

        # 2. POST /api/update-check with custom override
        status2, j2, _, _ = self.driver.request("POST", "/api/update-check", data={
            "repo": "iranl/harmony-ng",
            "remote_version": "2.1.1"
        })
        self.assertEqual(status2, 200)
        self.assertTrue(j2.get("ok"))

    def test_live_network_and_bluetooth_telemetry(self):
        """Test /api/network-status and /api/bt-status."""
        self.require_live_hub()
        
        status, net_j, _, _ = self.driver.request("GET", "/api/network-status")
        self.assertEqual(status, 200)
        self.assertTrue(net_j.get("ok"))

        status, bt_j, _, _ = self.driver.request("GET", "/api/bt-status")
        self.assertIn(status, (200, 404, 503))

    def test_live_custom_icons_and_layout(self):
        """Test /api/icons, /api/custom-icons, and /api/ui-remote-layout."""
        self.require_live_hub()
        
        status, icons, _, _ = self.driver.request("GET", "/api/icons")
        self.assertEqual(status, 200)
        self.assertIsInstance(icons, list)

        status, cust_icons, _, _ = self.driver.request("GET", "/api/custom-icons")
        self.assertEqual(status, 200)
        self.assertIsInstance(cust_icons, list)

        status, layout, _, _ = self.driver.request("GET", "/api/ui-remote-layout")
        self.assertEqual(status, 200)


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Harmony Hub OpenAPI Test Suite")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"Hub IP address (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"Hub port (default: {DEFAULT_PORT})")
    parser.add_argument("--spec", default=DEFAULT_SPEC, help="Path to openapi.yaml")
    parser.add_argument("--strict", action="store_true", help="Fail if live Hub is unreachable")
    args, unknown = parser.parse_known_args()

    os.environ["HUB_HOST"] = args.host
    os.environ["HUB_PORT"] = str(args.port)
    os.environ["OPENAPI_SPEC"] = args.spec

    driver = OpenApiDriver(host=args.host, port=args.port)
    is_live = driver.is_reachable()

    print(f"\n{C_BOLD}======================================================={C_RESET}")
    print(f"{C_BOLD}  Harmony Hub OpenAPI Test Suite                       {C_RESET}")
    print(f"{C_BOLD}======================================================={C_RESET}")
    print(f"  Spec Path: {args.spec}")
    print(f"  Target:    http://{args.host}:{args.port} ({C_GREEN}ONLINE{C_RESET}" if is_live else f"Target:    http://{args.host}:{args.port} ({C_YELLOW}OFFLINE / Mock Mode{C_RESET})")
    print("-------------------------------------------------------\n")

    if args.strict and not is_live:
        print(f"{C_RED}[ERROR] Strict mode enabled and Hub at {args.host}:{args.port} is unreachable.{C_RESET}")
        sys.exit(1)

    suite = unittest.TestLoader().loadTestsFromTestCase(TestOpenApiCompliance)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)

if __name__ == "__main__":
    main()
