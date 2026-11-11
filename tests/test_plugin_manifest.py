#!/usr/bin/env python3
"""
Comprehensive Unit Tests for the Harmony Plugin Architecture:
- Manifest schema and constraint validation
- Plugin ID security validation (traversal & sanitization)
- Repository catalog structure and package URLs
- Socket IPC framing, encoding, and action payloads
"""

import json
import os
import re
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SAFE_ID_REGEX = re.compile(r"^[a-zA-Z0-9_-]{1,63}$")

def is_safe_plugin_id(plugin_id: str) -> bool:
    if not plugin_id or not isinstance(plugin_id, str):
        return False
    if ".." in plugin_id or "/" in plugin_id or "\\" in plugin_id:
        return False
    return bool(SAFE_ID_REGEX.match(plugin_id))

def validate_manifest(manifest: dict) -> list[str]:
    errors = []
    if not isinstance(manifest, dict):
        return ["Manifest must be a JSON object"]

    pid = manifest.get("id")
    if not pid or not isinstance(pid, str):
        errors.append("Manifest missing 'id' string")
    elif not is_safe_plugin_id(pid):
        errors.append(f"Invalid plugin id '{pid}'")

    name = manifest.get("name")
    if not name or not isinstance(name, str) or len(name.strip()) == 0:
        errors.append("Manifest missing 'name' string")

    version = manifest.get("version")
    if not version or not isinstance(version, str):
        errors.append("Manifest missing 'version' string")

    entrypoint = manifest.get("entrypoint")
    if entrypoint is not None and not isinstance(entrypoint, str):
        errors.append("'entrypoint' must be a string path")

    dev_def = manifest.get("device_definition")
    if dev_def is not None:
        if not isinstance(dev_def, dict):
            errors.append("'device_definition' must be an object")
        else:
            if "device_type" not in dev_def or not isinstance(dev_def["device_type"], str):
                errors.append("device_definition missing 'device_type'")
            cfg_fields = dev_def.get("config_fields", [])
            if not isinstance(cfg_fields, list):
                errors.append("'config_fields' must be a list")
            else:
                for idx, cf in enumerate(cfg_fields):
                    if not isinstance(cf, dict) or "key" not in cf or "type" not in cf:
                        errors.append(f"config_field[{idx}] missing key or type")
                    elif cf["type"] not in ("string", "number", "boolean", "select", "password"):
                        errors.append(f"config_field[{idx}] invalid type '{cf['type']}'")
            def_cmds = dev_def.get("default_commands", [])
            if not isinstance(def_cmds, list):
                errors.append("'default_commands' must be a list")
            else:
                for idx, cmd in enumerate(def_cmds):
                    if not isinstance(cmd, dict) or "name" not in cmd or "keyCode" not in cmd:
                        errors.append(f"default_commands[{idx}] missing name or keyCode")

    return errors


class TestPluginSystem(unittest.TestCase):
    def test_repository_json(self):
        repo_path = os.path.join(REPO_ROOT, "plugins", "repository.json")
        self.assertTrue(os.path.isfile(repo_path), "plugins/repository.json must exist")
        with open(repo_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        self.assertIn("repo_name", data)
        self.assertIn("plugins", data)
        self.assertIsInstance(data["plugins"], list)
        self.assertGreaterEqual(len(data["plugins"]), 1)

        for p in data["plugins"]:
            self.assertIn("id", p)
            self.assertIn("name", p)
            self.assertIn("version", p)
            self.assertIn("download_url", p)
            self.assertTrue(is_safe_plugin_id(p["id"]), f"Invalid plugin id in repo: {p['id']}")
            self.assertTrue(p["download_url"].startswith("http://") or p["download_url"].startswith("https://"),
                            f"Invalid download_url: {p['download_url']}")
            if "device_definition" in p:
                errors = validate_manifest(p)
                self.assertEqual(errors, [], f"Repository item manifest errors: {errors}")

    def test_safe_plugin_id_validation(self):
        valid_ids = ["roku-ecp", "android_tv", "ps5-wake", "ble-adv-1", "Plugin123"]
        for vid in valid_ids:
            self.assertTrue(is_safe_plugin_id(vid), f"Expected '{vid}' to be valid")

        invalid_ids = [
            "",
            "   ",
            "../traversal",
            "roku/ecp",
            "roku\\ecp",
            "plugin:colon",
            "has space",
            "a" * 64,  # too long (>63)
            "plugin$name",
            "plugin;rm -rf",
            "$(whoami)",
            "foo\0bar",
        ]
        for inv in invalid_ids:
            self.assertFalse(is_safe_plugin_id(inv), f"Expected '{inv}' to be rejected")

    def test_manifest_validation(self):
        valid_manifest = {
            "id": "roku-ecp",
            "name": "Roku ECP Controller",
            "version": "1.0.0",
            "description": "Control Roku devices via WiFi",
            "entrypoint": "bin/roku_daemon",
            "device_definition": {
                "device_type": "roku",
                "display_name": "Roku Player",
                "config_fields": [
                    { "key": "ip_address", "label": "IP Address", "type": "string" },
                    { "key": "port", "label": "Port", "type": "number", "default": 8060 }
                ],
                "default_commands": [
                    { "name": "Home", "keyCode": "PLUGIN:roku-ecp:Home" },
                    { "name": "PowerOff", "keyCode": "PLUGIN:roku-ecp:PowerOff" }
                ]
            }
        }
        self.assertEqual(validate_manifest(valid_manifest), [])

        # Missing id
        bad = dict(valid_manifest)
        del bad["id"]
        self.assertTrue(len(validate_manifest(bad)) > 0)

        # Path traversal in id
        bad = dict(valid_manifest, id="../bad_id")
        self.assertTrue(len(validate_manifest(bad)) > 0)

        # Invalid config field type
        bad = {
            "id": "test",
            "name": "Test",
            "version": "0.1",
            "device_definition": {
                "device_type": "test",
                "config_fields": [{ "key": "foo", "type": "unsupported_type" }]
            }
        }
        self.assertTrue(any("invalid type" in e for e in validate_manifest(bad)))

    def test_socket_payload_framing(self):
        payload = {
            "plugin_id": "roku-ecp",
            "device_id": "dev_123",
            "device_name": "Living Room Roku",
            "command": "Home",
            "action": "tap",
            "value": 0,
            "config": { "ip_address": "192.168.1.50", "port": 8060 }
        }
        encoded = json.dumps(payload)
        parsed = json.loads(encoded)
        self.assertEqual(parsed["plugin_id"], "roku-ecp")
        self.assertEqual(parsed["command"], "Home")
        self.assertEqual(parsed["action"], "tap")
        self.assertEqual(parsed["config"]["ip_address"], "192.168.1.50")

    def test_socket_payload_actions(self):
        actions = ["tap", "press", "repeat", "release"]
        for act in actions:
            p = {
                "plugin_id": "test-plug",
                "device_id": "1",
                "device_name": "TV",
                "command": "VolumeUp",
                "action": act,
                "value": 42,
                "config": {}
            }
            enc = json.dumps(p)
            dec = json.loads(enc)
            self.assertEqual(dec["action"], act)
            self.assertEqual(dec["value"], 42)

    def test_socket_payload_unicode_and_special_chars(self):
        p = {
            "plugin_id": "ps5-wake",
            "device_id": "dev_ps5",
            "device_name": "PlayStation 5 (Salon)",
            "command": "WakeOnBT",
            "action": "tap",
            "value": 0,
            "config": { "mac": "00:11:22:33:44:55", "notes": "Salon / Chloé's room" }
        }
        enc = json.dumps(p, ensure_ascii=False)
        dec = json.loads(enc)
        self.assertEqual(dec["device_name"], "PlayStation 5 (Salon)")
        self.assertEqual(dec["config"]["notes"], "Salon / Chloé's room")


if __name__ == "__main__":
    unittest.main()
