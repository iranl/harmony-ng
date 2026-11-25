#!/usr/bin/env python3
"""
The infrared alternative of a plugin device's command (docs/PLUGINS.md): a command with
"irKeyCode" and "irWhen" goes out as infrared while the device's config field named by irWhen is
on, and to the plugin otherwise. Daemon in --dry-run: which way a command went is read from its
log ("IR Keycode simulated" or "Plugin command simulated").

- config field off or missing: the command goes to the plugin
- config field on (true, "true", 1): infrared, with the code, the ports and the press/release of
  an IR device's command; a plain command of the same device still goes to the plugin; a command
  with irKeyCode but no irWhen goes to the plugin
- the device's type says "Bluetooth" (a send path that would take it for a Bluetooth key)
- the fields survive a device save (also one that sends the commands back without them), a
  command save, a device created with the manifest's default_commands, and a reload of the config
- a boolean config field of the manifest gets false as its default
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8954
PID = "samsung-tv"
IR_HDMI2 = "G:GoVideoO1 32 Bit:()(0xE0E07D82)():3"
IR_SOURCE = "G:GoVideoO1 32 Bit:()(0xE0E0807F)():3"
DEV = "70"

failures = []


def check(cond, name, detail=""):
    print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
    if not cond:
        failures.append(name)


MANIFEST = {
    "id": PID,
    "name": "Samsung TV",
    "version": "1.0.0",
    "entrypoint": "bin/drv",
    "device_definition": {
        "device_type": "samsung_tv",
        "display_name": "Samsung TV",
        "config_fields": [
            {"key": "host", "label": "Host", "type": "string"},
            {"key": "hdmi_via_ir", "label": "Send HDMI keys as infrared", "type": "boolean"},
        ],
        "default_commands": [
            {"name": "HDMI 2", "keyCode": "PLUGIN:%s:KEY_HDMI2" % PID, "irKeyCode": IR_HDMI2, "irWhen": "hdmi_via_ir"},
            {"name": "Home", "keyCode": "PLUGIN:%s:KEY_HOME" % PID},
        ],
    },
}


def config_dir():
    d = tempfile.mkdtemp(prefix="plugin_ir_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devices["devices"].append({
        "id": DEV, "name": "Living TV", "type": "Bluetooth TV", "plugin": PID, "manufacturer": "Samsung",
        "config": {"host": "192.168.1.50"},
        "commands": [
            {"name": "HDMI 2", "keyCode": "PLUGIN:%s:KEY_HDMI2" % PID, "irKeyCode": IR_HDMI2, "irWhen": "hdmi_via_ir"},
            {"name": "Home", "keyCode": "PLUGIN:%s:KEY_HOME" % PID},
            {"name": "Source", "keyCode": "PLUGIN:%s:KEY_SOURCE" % PID, "irKeyCode": IR_SOURCE},
            {"name": "Level", "keyCode": "PLUGIN:%s:KEY_LEVEL" % PID, "range": [0, 100],
             "irKeyCode": IR_SOURCE, "irWhen": "hdmi_via_ir"},
        ],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    for name in ("activities.json", "settings.json"):
        shutil.copy(os.path.join(SAMPLE_CONFIG, name), os.path.join(d, name))
    return d


def install_manifest(hub):
    pdir = os.path.join(hub.data_dir, "plugins", PID)
    os.makedirs(os.path.join(pdir, "bin"), exist_ok=True)
    with open(os.path.join(pdir, "manifest.json"), "w") as f:
        json.dump(MANIFEST, f)
    with open(os.path.join(pdir, "bin", "drv"), "w") as f:
        f.write("#!/bin/sh\nexit 0\n")
    os.chmod(os.path.join(pdir, "bin", "drv"), 0o755)


def log_len(path):
    return len(open(path, errors="replace").read())


def log_since(path, mark):
    return open(path, errors="replace").read()[mark:]


def send(hub, log_path, command, action=None, dev=DEV):
    """One ir-send; returns (status, the log lines it wrote). 0.3 s apart: repeated presses of
    one key within 90 ms are dropped as a bounce."""
    time.sleep(0.3)
    mark = log_len(log_path)
    body = {"deviceId": dev, "command": command}
    if action:
        body["action"] = action
    st, _ = hub.http_post("/api/ir-send", body, timeout=5)
    time.sleep(0.15)
    return st, log_since(log_path, mark)


def went_ir(log, code):
    return ("IR Keycode simulated: %s (" % code) in log and "Plugin command simulated" not in log


def went_plugin(log, sub):
    return ("Plugin command simulated: plugin=%s" % PID) in log and ("cmd=%s " % sub) in log and "IR Keycode simulated" not in log


def device(hub, dev_id=DEV):
    st, res = hub.http_get("/api/devices")
    return next((d for d in (res or {}).get("devices", []) if str(d.get("id")) == dev_id), {})


def command(dev, name):
    return next((c for c in dev.get("commands", []) if c.get("name") == name), {})


def ws_ask(ws, req, timeout=5.0):
    ws.send_json(req)
    end = time.time() + timeout
    while time.time() < end:
        m = ws.recv_json(timeout=max(0.1, end - time.time()))
        if m and m.get("type") == "ack" and m.get("msgId") == req.get("msgId"):
            return m
    return None


def save_config(hub, value):
    cfg = {"host": "192.168.1.50"}
    if value is not None:
        cfg["hdmi_via_ir"] = value
    st, res = hub.http_post("/api/device-save", {"deviceId": DEV, "config": cfg})
    return st == 200


def main():
    print("[-] Plugin command with an infrared alternative (irKeyCode, irWhen)")
    cfg = config_dir()
    log_path = os.path.join(cfg, "hub.log")
    reload_dir = tempfile.mkdtemp(prefix="plugin_ir_reload_")
    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log_path) as hub:
            install_manifest(hub)

            dev = device(hub)
            hd = command(dev, "HDMI 2")
            check(hd.get("irKeyCode") == IR_HDMI2 and hd.get("irWhen") == "hdmi_via_ir",
                  "irKeyCode and irWhen kept at load", str(hd))

            # 1. switch missing, then false: the plugin
            st, log = send(hub, log_path, "HDMI 2")
            check(st == 200 and went_plugin(log, "KEY_HDMI2"), "switch missing: HDMI 2 goes to the plugin", log)
            check(save_config(hub, False), "device saved with hdmi_via_ir false")
            st, log = send(hub, log_path, "HDMI 2")
            check(st == 200 and went_plugin(log, "KEY_HDMI2"), "switch false: HDMI 2 goes to the plugin", log)

            # 2. switch on: infrared, as an IR device's command (all ports: no port, no controlPort)
            check(save_config(hub, True), "device saved with hdmi_via_ir true")
            st, log = send(hub, log_path, "HDMI 2")
            check(st == 200 and went_ir(log, IR_HDMI2), "switch true: HDMI 2 goes out as infrared", log)
            check("port_mask 0x07" in log, "infrared on all blasters", log)
            st, log = send(hub, log_path, "Home")
            check(st == 200 and went_plugin(log, "KEY_HOME"), "a plain command still goes to the plugin", log)
            st, log = send(hub, log_path, "Source")
            check(st == 200 and went_plugin(log, "KEY_SOURCE"), "irKeyCode without irWhen goes to the plugin", log)

            # press and release as for an IR device: the press sends, the release sends nothing
            st, log = send(hub, log_path, "HDMI 2", "press")
            check(st == 200 and went_ir(log, IR_HDMI2), "press goes out as infrared", log)
            st, log = send(hub, log_path, "HDMI 2", "release")
            check(st == 200 and "Plugin command simulated" not in log and "IR Keycode simulated" not in log,
                  "release of the infrared key reaches no plugin", log)

            # other values that turn it on or off
            for value, ir in (("true", True), ("1", True), (1, True), ("0", False), (0, False), ("no", False)):
                save_config(hub, value)
                st, log = send(hub, log_path, "HDMI 2")
                check(went_ir(log, IR_HDMI2) if ir else went_plugin(log, "KEY_HDMI2"),
                      "hdmi_via_ir %r: %s" % (value, "infrared" if ir else "plugin"), log)
            save_config(hub, True)

            # a command with a range and an active infrared alternative needs no value: the
            # infrared send takes none (single send and the queued multi-command path)
            st, log = send(hub, log_path, "Level")
            check(st == 200 and went_ir(log, IR_SOURCE), "ranged command, switch on: sent as infrared without a value", log)
            ws = hub.ws()
            mark = log_len(log_path)
            r = ws_ask(ws, {"action": "send_command", "msgId": 31, "deviceId": DEV, "command": ["Level", "Home"]})
            time.sleep(1.0)
            log = log_since(log_path, mark)
            check(r is not None and r.get("ok") is True, "queued ranged command, switch on: accepted without a value", str(r))
            check(("IR Keycode simulated: %s (" % IR_SOURCE) in log, "queued ranged command went out as infrared", log)
            save_config(hub, False)
            r = ws_ask(ws, {"action": "send_command", "msgId": 32, "deviceId": DEV, "command": ["Level", "Home"]})
            check(r is not None and r.get("ok") is False and r.get("code") == 400,
                  "queued ranged command, switch off: still refused without a value (plugin)", str(r))
            ws.close()
            save_config(hub, True)

            # 3. a command's own port is used
            hd2 = dict(command(device(hub), "HDMI 2"))
            hd2["port"] = 2
            cmds = [hd2 if c.get("name") == "HDMI 2" else c for c in device(hub)["commands"]]
            st, _ = hub.http_post("/api/device-save", {"deviceId": DEV, "commands": cmds})
            st, log = send(hub, log_path, "HDMI 2")
            check(went_ir(log, IR_HDMI2) and "port_mask 0x02" in log, "the command's own port is used", log)

            # 4. a device save that sends the commands back without the fields keeps them
            bare = [{"name": c["name"], "keyCode": c["keyCode"]} for c in device(hub)["commands"]]
            st, _ = hub.http_post("/api/device-save", {"deviceId": DEV, "commands": bare})
            hd = command(device(hub), "HDMI 2")
            check(st == 200 and hd.get("irKeyCode") == IR_HDMI2 and hd.get("irWhen") == "hdmi_via_ir",
                  "a save of the commands without the fields keeps them", str(hd))
            # a save that changes only irKeyCode keeps the stored irWhen, and the other way round
            part = [dict(c) for c in device(hub)["commands"]]
            for c in part:
                if c.get("name") == "HDMI 2":
                    c.pop("irWhen", None)
                    c["irKeyCode"] = IR_SOURCE
            st, _ = hub.http_post("/api/device-save", {"deviceId": DEV, "commands": part})
            hd = command(device(hub), "HDMI 2")
            check(st == 200 and hd.get("irKeyCode") == IR_SOURCE and hd.get("irWhen") == "hdmi_via_ir",
                  "a save with irKeyCode only keeps the stored irWhen", str(hd))
            part = [dict(c) for c in device(hub)["commands"]]
            for c in part:
                if c.get("name") == "HDMI 2":
                    c.pop("irKeyCode", None)
            st, _ = hub.http_post("/api/device-save", {"deviceId": DEV, "commands": part})
            hd = command(device(hub), "HDMI 2")
            check(st == 200 and hd.get("irKeyCode") == IR_SOURCE and hd.get("irWhen") == "hdmi_via_ir",
                  "a save with irWhen only keeps the stored irKeyCode", str(hd))
            # ... and null removes them
            st, _ = hub.http_post("/api/device-save", {"deviceId": DEV, "commands": [
                {"name": "HDMI 2", "keyCode": "PLUGIN:%s:KEY_HDMI2" % PID, "irKeyCode": IR_HDMI2, "irWhen": "hdmi_via_ir"},
                {"name": "Home", "keyCode": "PLUGIN:%s:KEY_HOME" % PID},
                {"name": "Source", "keyCode": "PLUGIN:%s:KEY_SOURCE" % PID, "irKeyCode": None},
            ]})
            src = command(device(hub), "Source")
            check("irKeyCode" not in src and "port" not in src and "repeats" not in src,
                  "irKeyCode null removes it (and the IR fields)", str(src))

            # 5. command save: a new command with the fields; a preset added by name gets the manifest's
            st, _ = hub.http_post("/api/command-save", {"deviceId": DEV, "name": "HDMI 3", "raw": "PLUGIN:%s:KEY_HDMI3" % PID,
                                                         "irKeyCode": IR_SOURCE, "irWhen": "hdmi_via_ir"})
            h3 = command(device(hub), "HDMI 3")
            check(st == 200 and h3.get("irKeyCode") == IR_SOURCE and h3.get("irWhen") == "hdmi_via_ir",
                  "command-save keeps irKeyCode and irWhen", str(h3))
            st, log = send(hub, log_path, "HDMI 3")
            check(went_ir(log, IR_SOURCE), "a command saved with the fields goes out as infrared", log)
            st, _ = hub.http_post("/api/commands-batch", {"deviceId": DEV, "commands": [
                {"name": "HDMI 4", "raw": "PLUGIN:%s:KEY_HDMI4" % PID, "irKeyCode": IR_SOURCE, "irWhen": "hdmi_via_ir"}]})
            h4 = command(device(hub), "HDMI 4")
            check(st == 200 and h4.get("irKeyCode") == IR_SOURCE and h4.get("irWhen") == "hdmi_via_ir",
                  "commands-batch keeps irKeyCode and irWhen", str(h4))
            # the legacy UI's preset Add sends name and keyCode only
            hub.http_post("/api/device-save", {"deviceId": DEV, "commands": [{"name": "Home", "keyCode": "PLUGIN:%s:KEY_HOME" % PID}]})
            st, _ = hub.http_post("/api/command-save", {"deviceId": DEV, "name": "HDMI 2", "raw": "PLUGIN:%s:KEY_HDMI2" % PID,
                                                         "mode": "plugin"})
            hd = command(device(hub), "HDMI 2")
            check(hd.get("irKeyCode") == IR_HDMI2 and hd.get("irWhen") == "hdmi_via_ir",
                  "a preset added by name takes the manifest's irKeyCode and irWhen", str(hd))

            # 6. a new device with the manifest's default_commands (the new web UI sends them)
            st, res = hub.http_post("/ir/new-device", {"name": "Bedroom TV", "type": "plugin:%s" % PID, "plugin": PID,
                                                       "config": json.dumps({"host": "192.168.1.51"}),
                                                       "commands": json.dumps(MANIFEST["device_definition"]["default_commands"])})
            nid = str(res.get("deviceId") or res.get("id")) if isinstance(res, dict) else ""
            hd = command(device(hub, nid), "HDMI 2")
            check(st == 200 and hd.get("irKeyCode") == IR_HDMI2 and hd.get("irWhen") == "hdmi_via_ir",
                  "new device with default_commands keeps the fields", str(hd))
            # ... and one created without commands is filled from the manifest
            st, res = hub.http_post("/ir/new-device", {"name": "Kitchen TV", "type": "plugin:%s" % PID, "plugin": PID})
            kid = str(res.get("deviceId") or res.get("id")) if isinstance(res, dict) else ""
            kdev = device(hub, kid)
            hd = command(kdev, "HDMI 2")
            check(hd.get("irKeyCode") == IR_HDMI2 and hd.get("irWhen") == "hdmi_via_ir",
                  "new device filled from the manifest keeps the fields", str(hd))
            check(kdev.get("config", {}).get("hdmi_via_ir") is False,
                  "a boolean config field defaults to false", str(kdev.get("config")))
            st, log = send(hub, log_path, "HDMI 2", dev=kid)
            check(st == 200 and went_plugin(log, "KEY_HDMI2"), "its HDMI 2 goes to the plugin (switch off)", log)

            for name in ("devices.json", "activities.json", "settings.json"):
                shutil.copy(os.path.join(hub.data_dir, "config", name), os.path.join(reload_dir, name))

        # 7. reload of the saved config
        log2 = os.path.join(reload_dir, "hub.log")
        with HubHarness(port=PORT, config_dir=reload_dir, log_path=log2) as hub:
            hd = command(device(hub), "HDMI 2")
            check(hd.get("irKeyCode") == IR_HDMI2 and hd.get("irWhen") == "hdmi_via_ir",
                  "irKeyCode and irWhen kept over a reload", str(hd))
            st, log = send(hub, log2, "HDMI 2")
            check(st == 200 and went_ir(log, IR_HDMI2), "after a reload HDMI 2 still goes out as infrared", log)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(reload_dir, ignore_errors=True)

    if failures:
        print("[!] %d check(s) failed" % len(failures))
        sys.exit(1)
    print("[+] Plugin command infrared alternative: all checks passed")


if __name__ == "__main__":
    main()
