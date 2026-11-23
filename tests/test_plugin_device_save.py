#!/usr/bin/env python3
"""
Unit tests for Plugin System Fixes:
1. Saving devices as plugin device type (clean schema, config persistence, no controlPort, no port/repeats on commands).
2. Storage metrics and file sizes on GET /api/plugins.
3. Prevent deleting/uninstalling plugins currently setup as a device.
4. Dev version direct download from raw manifest URL.
5. Review of PR #243: a device belongs to a plugin only through "plugin" / "plugin:<id>" (an IR
   device whose type matches a plugin name stays IR), unsafe plugin ids refused, unknown keys not
   copied into config, the action route reply, installs refused while authentication is off.
"""

import http.server
import json
import os
import shutil
import socketserver
import sys
import threading
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness

PORT = 8945
PORT_NOAUTH = 8946
failed = 0
# Authentication on: plugin installs need it. 127.0.0.1 is trusted as admin, so the requests
# below need no credentials.
USERS = {"auth_enabled": True, "users": [{"username": "admin", "password": "secret123", "role": "admin"}]}


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def main():
    print("[-] Running Plugin Device Save & Architecture Tests...")

    # Authentication off (the default): installs, pulls and repository changes are refused
    with HubHarness(port=PORT_NOAUTH) as hub:
        for path, body in (("/api/plugins/install", {"url": "http://127.0.0.1:9/p.tar.gz", "sha256": "0" * 64}),
                           ("/api/plugins/pull", {"id": "x"}),
                           ("/api/plugins/repositories", {"action": "add", "url": "https://example.com/r.json"})):
            st, res = hub.http_post(path, body)
            check(st == 403 and "authentication" in str(res.get("error", "")).lower(), "%s refused while auth is off" % path, str((st, res)))
        st, _ = hub.http_get("/api/plugins")
        check(st == 200, "GET /api/plugins still answers while auth is off")

    with HubHarness(port=PORT, extra_config_files={"users.json": USERS, "repositories.json": []}) as hub:
        # Create mock installed plugin 'philips-hue'
        plug_dir = os.path.join(hub.data_dir, "plugins", "philips-hue")
        os.makedirs(plug_dir, exist_ok=True)
        manifest_data = {
            "id": "philips-hue",
            "name": "Philips Hue Bridge",
            "version": "1.0.0",
            "entrypoint": "bin/drv",
            "device_definition": {
                "device_type": "hue_light",
                "display_name": "Hue Bridge",
                "config_fields": [
                    {"key": "bridge_ip", "label": "Bridge IP", "type": "string"},
                    {"key": "api_key", "label": "API Key", "type": "string"}
                ],
                "default_commands": [
                    {"name": "PowerToggle", "keyCode": "PLUGIN:philips-hue:PowerToggle"},
                    {"name": "BrightUp", "keyCode": "PLUGIN:philips-hue:BrightUp"}
                ]
            }
        }
        with open(os.path.join(plug_dir, "manifest.json"), "w") as mf:
            json.dump(manifest_data, mf)
        os.makedirs(os.path.join(plug_dir, "bin"), exist_ok=True)
        with open(os.path.join(plug_dir, "bin", "drv"), "w") as rf:
            rf.write("#!/bin/sh\nexit 0\n")
        os.chmod(os.path.join(plug_dir, "bin", "drv"), 0o755)

        # -------------------------------------------------------------
        # Test 1: GET /api/plugins returns storage and plugin file size
        # -------------------------------------------------------------
        st, res = hub.http_get("/api/plugins")
        check(st == 200, "GET /api/plugins succeeds")
        plugs = res.get("plugins", [])
        hue_plug = next((p for p in plugs if p.get("id") == "philips-hue"), None)
        check(hue_plug is not None, "philips-hue found in installed plugins")
        check("size_bytes" in hue_plug and hue_plug["size_bytes"] > 0, "size_bytes telemetry present and > 0")
        check("size_formatted" in hue_plug and len(hue_plug["size_formatted"]) > 0, "size_formatted string present")

        storage = res.get("storage", {})
        check("total_bytes" in storage and storage["total_bytes"] > 0, "storage total_bytes > 0")
        check("free_bytes" in storage and storage["free_bytes"] > 0, "storage free_bytes > 0")
        check("free_formatted" in storage, "storage free_formatted present")

        # -------------------------------------------------------------
        # Test 2: Save new device as plugin device type via POST /ir/new-device
        # -------------------------------------------------------------
        new_dev_payload = {
            "name": "Living Room Hue",
            "type": "plugin:philips-hue",
            "plugin": "philips-hue",
            "manufacturer": "Philips",
            "model": "Hue Light",
            "config": json.dumps({"bridge_ip": "192.168.1.100", "api_key": "test_token_123"}),
            "commands": json.dumps([
                {"name": "PowerToggle", "keyCode": "PLUGIN:philips-hue:PowerToggle", "raw": "PLUGIN:philips-hue:PowerToggle"},
                {"name": "BrightUp", "keyCode": "PLUGIN:philips-hue:BrightUp", "raw": "PLUGIN:philips-hue:BrightUp"}
            ])
        }
        st, res = hub.http_post("/ir/new-device", new_dev_payload)
        check(st == 200 and isinstance(res, dict) and res.get("ok") is True, "POST /ir/new-device saves plugin device")
        dev_id = str(res.get("deviceId") or res.get("id"))
        check(len(dev_id) > 0, f"Assigned deviceId {dev_id}")

        # Verify devices.json
        st, dev_res = hub.http_get("/api/devices")
        check(st == 200, "GET /api/devices succeeds")
        devices = dev_res.get("devices", [])
        saved_dev = next((d for d in devices if str(d.get("id")) == dev_id), None)
        check(saved_dev is not None, "Saved plugin device appears in inventory")
        check(saved_dev.get("type") == "plugin:philips-hue", "Device type preserved as plugin:philips-hue")
        check(saved_dev.get("plugin") == "philips-hue", "Device plugin id preserved as philips-hue")
        check("controlPort" not in saved_dev, "controlPort correctly excluded from plugin device")

        # Config verified
        cfg = saved_dev.get("config", {})
        check(cfg.get("bridge_ip") == "192.168.1.100", "Plugin config bridge_ip correctly parsed")
        check(cfg.get("api_key") == "test_token_123", "Plugin config api_key correctly parsed")

        # Commands verified (no port, no repeats)
        cmds = saved_dev.get("commands", [])
        check(len(cmds) == 2, f"Plugin commands preserved (count: {len(cmds)})")
        for cmd in cmds:
            check("port" not in cmd, f"Command '{cmd.get('name')}' has no IR port attribute")
            check("repeats" not in cmd, f"Command '{cmd.get('name')}' has no IR repeats attribute")
            check(cmd.get("keyCode", "").startswith("PLUGIN:philips-hue:"), f"Command '{cmd.get('name')}' has valid keyCode")

        # -------------------------------------------------------------
        # Test 2b: Update plugin device settings via POST /ir/device
        # -------------------------------------------------------------
        update_dev_payload = {
            "id": dev_id,
            "deviceId": dev_id,
            "name": "Updated Hue Bridge",
            "type": "plugin:philips-hue",
            "plugin": "philips-hue",
            "config": json.dumps({"bridge_ip": "192.168.1.222", "api_key": "new_secret_key"}),
        }
        st, res = hub.http_post("/ir/device", update_dev_payload)
        check(st == 200 and res.get("ok") is True, "POST /ir/device updates plugin device settings")

        st, dev_res2 = hub.http_get("/api/devices")
        check(st == 200, "GET /api/devices succeeds after update")
        updated_dev = next((d for d in dev_res2.get("devices", []) if str(d.get("id")) == dev_id), None)
        check(updated_dev is not None, "Updated device found in inventory")
        check(updated_dev.get("name") == "Updated Hue Bridge", "Device name updated")
        check(updated_dev.get("type") == "plugin:philips-hue", "Device type preserved as plugin:philips-hue")
        check(updated_dev.get("plugin") == "philips-hue", "Device plugin preserved as philips-hue")
        updated_cfg = updated_dev.get("config", {})
        check(updated_cfg.get("bridge_ip") == "192.168.1.222", f"Config bridge_ip updated: {updated_cfg}")
        check(updated_cfg.get("api_key") == "new_secret_key", f"Config api_key updated: {updated_cfg}")
        # Flat top-level keys: only those the plugin's config_fields declare reach config
        st, res = hub.http_post("/ir/device", {"deviceId": dev_id, "plugin": "philips-hue", "bridge_ip": "10.0.0.9",
                                               "room": "living", "name": "Updated Hue Bridge"})
        check(st == 200, "Flat plugin field saved")
        st, dev_res3 = hub.http_get("/api/devices")
        cfg3 = next((d for d in dev_res3.get("devices", []) if str(d.get("id")) == dev_id), {}).get("config", {})
        check(cfg3.get("bridge_ip") == "10.0.0.9", "Declared flat key bridge_ip copied into config", str(cfg3))
        check("room" not in cfg3, "Undeclared flat key room not copied into config", str(cfg3))

        # The action route answers with the id and action it was given (they pointed into freed JSON)
        st, act = hub.http_post("/api/plugins/action", {"id": "philips-hue", "action": "stop"})
        check(st == 200 and act.get("id") == "philips-hue" and act.get("action") == "stop", "Action reply carries id and action", str(act))

        st, del_res = hub.http_post("/api/plugins/uninstall", {"id": "philips-hue"})
        check(st == 400, "Uninstalling plugin in use by device rejected with HTTP 400")
        check("currently in use" in del_res.get("error", "").lower() or "in use" in del_res.get("message", "").lower(),
              "Error message indicates plugin is in use", f"Got: {del_res}")
        check(os.path.exists(plug_dir), "Plugin files remain intact after rejected uninstall")

        # -------------------------------------------------------------
        # Test 3: Save Roku device with type "Roku"
        # -------------------------------------------------------------
        roku_plug_dir = os.path.join(hub.data_dir, "plugins", "roku-ecp")
        os.makedirs(roku_plug_dir, exist_ok=True)
        roku_manifest = {
            "id": "roku-ecp",
            "name": "Roku ECP Controller",
            "version": "1.0.0",
            "entrypoint": "bin/drv",
            "device_definition": {
                "device_type": "roku",
                "display_name": "Roku Player / TV",
                "config_fields": [
                    {"key": "ip_address", "label": "IP Address", "type": "string"},
                    {"key": "port", "label": "Port", "type": "number", "default": 8060}
                ],
                "default_commands": [
                    {"name": "Home", "keyCode": "PLUGIN:roku-ecp:Home"},
                    {"name": "Back", "keyCode": "PLUGIN:roku-ecp:Back"},
                    {"name": "Select", "keyCode": "PLUGIN:roku-ecp:Select"},
                    {"name": "DirectionUp", "keyCode": "PLUGIN:roku-ecp:Up"},
                    {"name": "DirectionDown", "keyCode": "PLUGIN:roku-ecp:Down"},
                    {"name": "DirectionLeft", "keyCode": "PLUGIN:roku-ecp:Left"},
                    {"name": "DirectionRight", "keyCode": "PLUGIN:roku-ecp:Right"},
                    {"name": "Play", "keyCode": "PLUGIN:roku-ecp:Play"},
                    {"name": "Rev", "keyCode": "PLUGIN:roku-ecp:Rev"},
                    {"name": "Fwd", "keyCode": "PLUGIN:roku-ecp:Fwd"},
                    {"name": "PowerOff", "keyCode": "PLUGIN:roku-ecp:PowerOff"}
                ]
            }
        }
        with open(os.path.join(roku_plug_dir, "manifest.json"), "w") as rf:
            json.dump(roku_manifest, rf)
        os.makedirs(os.path.join(roku_plug_dir, "bin"), exist_ok=True)
        with open(os.path.join(roku_plug_dir, "bin", "drv"), "w") as rf:
            rf.write("#!/bin/sh\nexit 0\n")
        os.chmod(os.path.join(roku_plug_dir, "bin", "drv"), 0o755)

        # A device whose free-text type matches a plugin's name is not that plugin's device
        tv_plug_dir = os.path.join(hub.data_dir, "plugins", "thirdparty")
        os.makedirs(os.path.join(tv_plug_dir, "bin"), exist_ok=True)
        with open(os.path.join(tv_plug_dir, "manifest.json"), "w") as f:
            json.dump({"id": "thirdparty", "version": "1", "entrypoint": "bin/drv",
                       "device_definition": {"device_type": "television", "display_name": "television"}}, f)
        st, res = hub.http_post("/api/ir-send", {"deviceId": "1", "command": "PowerOn"}, timeout=5)
        check("plugin" not in json.dumps(res).lower(), "IR device of type Television still sends IR", str(res))
        st, res = hub.http_post("/api/device-save", {"deviceId": "1", "name": "Living Room TV", "type": "Television",
                                                     "defaultInterDeviceDelay": 500, "room": "living"})
        check(st == 200, "Unchanged IR device saved")
        st, devs = hub.http_get("/api/devices")
        tv = next((d for d in devs.get("devices", []) if str(d.get("id")) == "1"), {})
        check("plugin" not in tv and tv.get("controlPort") == 7, "IR device keeps no plugin and its controlPort", str(tv))
        check("config" not in tv, "Unknown keys of a non-plugin device not copied into config", str(tv.get("config")))

        # A plugin id goes into a socket path: an unsafe one is refused
        st, res = hub.http_post("/api/device-save", {"deviceId": "1", "plugin": "../../tmp/evil"})
        check(st == 400, "Unsafe plugin id refused", str((st, res)))
        st, res = hub.http_post("/api/device-save", {"deviceId": "1", "type": "plugin:../x"})
        check(st == 400, "Unsafe plugin:<id> type refused", str((st, res)))

        # Plugin routes: unknown uninstall is 404, a URL install without a SHA-256 is refused
        st, res = hub.http_post("/api/plugins/uninstall", {"id": "not-installed"})
        check(st == 404, "Uninstall of a plugin that is not installed answers 404", str((st, res)))
        st, res = hub.http_post("/api/plugins/install", {"url": "http://127.0.0.1:9/p.tar.gz"})
        check(st == 400 and "SHA-256" in res.get("error", ""), "URL install without sha256 refused", str((st, res)))
        st, res = hub.http_post("/api/plugins/install", {"url": "http://127.0.0.1:9/m/manifest.json"})
        check(st == 400 and "SHA-256" in res.get("error", ""), "A manifest URL is no dev install unless dev is set", str((st, res)))
        st, res = hub.http_post("/api/plugins/install", {"archive_base64": "AAAA"})
        check(st == 400 and "sha256" in res.get("error", ""), "Upload without sha256 or dev refused", str((st, res)))

        roku_payload = {
            "name": "My Roku TV",
            "type": "Roku",
            "plugin": "roku-ecp",
            "manufacturer": "Roku",
            "model": "Streaming Stick",
            "config": json.dumps({"ip_address": "192.168.1.88", "port": 8060})
        }
        st, res = hub.http_post("/ir/new-device", roku_payload)
        check(st == 200 and res.get("ok") is True, "POST /ir/new-device saves Roku device with type Roku")
        roku_dev_id = str(res.get("deviceId") or res.get("id"))
        check(len(roku_dev_id) > 0, f"Assigned deviceId {roku_dev_id} for Roku")

        st, dev_res_r = hub.http_get("/api/devices")
        roku_saved = next((d for d in dev_res_r.get("devices", []) if str(d.get("id")) == roku_dev_id), None)
        check(roku_saved is not None, "Roku device found in inventory")
        check(roku_saved.get("type") in ("Roku", "plugin:roku-ecp"), f"Roku device type valid: {roku_saved.get('type')}")
        check(roku_saved.get("plugin") == "roku-ecp", f"Roku device plugin is roku-ecp: {roku_saved.get('plugin')}")
        check("controlPort" not in roku_saved, "controlPort not present on Roku device")
        r_cfg = roku_saved.get("config", {})
        check(r_cfg.get("ip_address") == "192.168.1.88", f"Roku IP address preserved: {r_cfg}")
        check(r_cfg.get("port") == 8060 or r_cfg.get("port") == "8060", f"Roku port preserved: {r_cfg}")
        r_cmds = roku_saved.get("commands", [])
        check(len(r_cmds) >= 10, f"Roku default commands auto-populated: {len(r_cmds)}")
        home_cmd = next((c for c in r_cmds if c.get("name") == "Home"), None)
        check(home_cmd is not None and "PLUGIN:roku-ecp:Home" in home_cmd.get("keyCode", ""), "Home command has valid plugin keyCode")
        st, del_roku_plug = hub.http_post("/api/plugins/uninstall", {"id": "roku-ecp"})
        check(st == 400, "Uninstalling roku-ecp rejected while Roku device exists")
        hub.http_post("/ir/delete-device", {"id": roku_dev_id, "deviceId": roku_dev_id})

        # A plugin device whose type or manufacturer says "Bluetooth" keeps its plugin (the
        # Bluetooth guess was made first and dropped it)
        st, res = hub.http_post("/ir/new-device", {"name": "BT Amp", "type": "Bluetooth Speaker", "plugin": "roku-ecp",
                                                   "manufacturer": "Bluetooth Amp Plugin", "config": {"ip_address": "10.0.0.5"}})
        bt_id = str(res.get("deviceId") or res.get("id"))
        st, devs = hub.http_get("/api/devices")
        bt_dev = next((d for d in devs.get("devices", []) if str(d.get("id")) == bt_id), {})
        check(bt_dev.get("plugin") == "roku-ecp" and "bdaddr" not in bt_dev, "Bluetooth-named plugin device keeps its plugin", str(bt_dev))
        # a save without "plugin" (a rename) keeps the plugin ...
        st, res = hub.http_post("/api/device-save", {"deviceId": bt_id, "name": "BT Amp 2"})
        st, devs = hub.http_get("/api/devices")
        bt_dev = next((d for d in devs.get("devices", []) if str(d.get("id")) == bt_id), {})
        check(bt_dev.get("plugin") == "roku-ecp", "A rename keeps the plugin", str(bt_dev))
        # ... and the form the legacy web UI sends, with an empty plugin=, makes it IR again
        form = urllib.parse.urlencode({"deviceId": bt_id, "name": "BT Amp", "type": "IR", "plugin": ""}).encode()
        st, res = hub.http_post("/ir/device", None, headers={"Content-Type": "application/x-www-form-urlencoded"}, raw=form)
        check(st == 200, "Plugin device saved back as IR", str((st, res)))
        st, devs = hub.http_get("/api/devices")
        bt_dev = next((d for d in devs.get("devices", []) if str(d.get("id")) == bt_id), {})
        check("plugin" not in bt_dev and not str(bt_dev.get("type", "")).startswith("plugin:"), "plugin \"\" clears the plugin", str(bt_dev))
        hub.http_post("/ir/delete-device", {"id": bt_id, "deviceId": bt_id})

        # stop of a plugin that is not installed: 404, no marker for any id a client names
        st, res = hub.http_post("/api/plugins/action", {"id": "never-installed", "action": "stop"})
        check(st == 404, "Stop of an unknown plugin answers 404", str((st, res)))

        # -------------------------------------------------------------
        # Test 4: Delete device, then uninstall plugin successfully
        # -------------------------------------------------------------
        st, del_dev_res = hub.http_post("/ir/delete-device", {"id": dev_id, "deviceId": dev_id})
        check(st == 200, "Deleting device succeeds")

        st, del_plug_res = hub.http_post("/api/plugins/uninstall", {"id": "philips-hue"})
        check(st == 200 and del_plug_res.get("ok") is True, "Uninstall succeeds once device is removed")
        check(not os.path.exists(plug_dir), "Plugin files removed after successful uninstall")

        # -------------------------------------------------------------
        # Test 5: Dev download from raw manifest URL
        # -------------------------------------------------------------
        mock_repo_dir = os.path.join(hub.mock_dir, "mock_raw_repo")
        os.makedirs(os.path.join(mock_repo_dir, "sub"), exist_ok=True)
        dev_manifest = {
            "id": "dev-tado",
            "name": "Dev Tado Smart AC",
            "version": "2.1.0",
            "files": ["manifest.json", "driver.sh", "sub/worker.sh"],
            "entrypoint": "driver.sh"
        }
        with open(os.path.join(mock_repo_dir, "manifest.json"), "w") as f:
            json.dump(dev_manifest, f)
        with open(os.path.join(mock_repo_dir, "driver.sh"), "w") as f:
            f.write("#!/bin/sh\necho tado\n")
        with open(os.path.join(mock_repo_dir, "sub", "worker.sh"), "w") as f:
            f.write("#!/bin/sh\necho worker\n")

        # Serve mock_repo_dir via local HTTP server
        handler = lambda *args, **kwargs: http.server.SimpleHTTPRequestHandler(*args, directory=mock_repo_dir, **kwargs)
        socketserver.TCPServer.allow_reuse_address = True
        httpd = socketserver.TCPServer(("127.0.0.1", 9993), handler)
        http_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        http_thread.start()
        time.sleep(0.2)

        try:
            dev_url = "http://127.0.0.1:9993/manifest.json"
            st, inst_res = hub.http_post("/api/plugins/install", {"url": dev_url, "dev": True})
            check(st == 200 and inst_res.get("ok") is True, "Dev direct file download succeeds")

            tado_dir = os.path.join(hub.data_dir, "plugins", "dev-tado")
            check(os.path.exists(os.path.join(tado_dir, "manifest.json")), "Dev manifest downloaded")
            check(os.path.exists(os.path.join(tado_dir, "driver.sh")), "Dev driver.sh downloaded")
            check(os.path.exists(os.path.join(tado_dir, "sub", "worker.sh")), "Dev sub/worker.sh downloaded")
            check(os.access(os.path.join(tado_dir, "driver.sh"), os.X_OK), "driver.sh is executable")
            check(os.access(os.path.join(tado_dir, "sub", "worker.sh"), os.X_OK), "sub/worker.sh is executable")
        finally:
            httpd.shutdown()
            httpd.server_close()

    if failed:
        print(f"\n[!] {failed} checks FAILED.")
        sys.exit(1)
    else:
        print("\n[+] All Plugin Device Save & Architecture Tests PASSED!")
        sys.exit(0)


if __name__ == "__main__":
    main()
