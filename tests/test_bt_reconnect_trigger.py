#!/usr/bin/env python3
"""
Bluetooth reconnect trigger of hw_action.c (check_and_trigger_bt_reconnect_for_command), on
hub_daemon_test in dry-run mode. The daemon logs one "BT reconnect trigger" line per device it
reconnects; the test reads the log after each POST /api/ir-send.

A Bluetooth device is reconnected when a command named by its btTriggerCommandName (optionally
"<deviceId>:<command>") is sent to the trigger device, or when a command is sent to the device
itself. Never for power-down commands, never for a device with btAutoReconnect 0, never while no
activity runs.

Replaces tests/test_bt_reconnect_trigger.c, which tested a copy of the function kept inside the
test file (the copy had already lost the "no activity" rule).
"""

import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 9266
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def devices_config():
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        cfg = json.load(f)
    cfg["devices"] += [
        {"id": "5", "name": "Stream Box", "type": "BT", "bdaddr": "00:11:22:33:44:55",
         "btTriggerDeviceId": "1", "btTriggerCommandName": "PowerOn", "btReconnectDelaySec": 0,
         "commands": [{"name": "Home", "keyCode": "HID:0C:0223"}]},
        {"id": "6", "name": "Game Pad", "type": "BT", "bdaddr": "00:11:22:33:44:66",
         "btAutoReconnect": 0, "btTriggerDeviceId": "1", "btTriggerCommandName": "PowerOn",
         "commands": [{"name": "Home", "keyCode": "HID:0C:0223"}]},
        {"id": "7", "name": "Audio Link", "type": "BT", "bdaddr": "00:11:22:33:44:77",
         "btTriggerCommandName": "1:InputHdmi1", "btReconnectDelaySec": 0,
         "commands": [{"name": "Home", "keyCode": "HID:0C:0223"}]},
    ]
    return cfg


class Log:
    def __init__(self, path):
        self.path = path
        self.pos = 0

    def new_lines(self):
        time.sleep(0.4)
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            data = f.read()
            self.pos = f.tell()
        return data.decode("utf-8", "replace")


def triggered(text):
    names = []
    for line in text.splitlines():
        if "BT reconnect trigger:" in line and "device='" in line:
            names.append(line.split("device='", 1)[1].split("'", 1)[0])
    return names


def main():
    log_path = tempfile.mktemp(prefix="bt_trigger_", suffix=".log")
    try:
        with HubHarness(port=PORT, log_path=log_path,
                        extra_config_files={"devices.json": devices_config()}) as hub:
            log = Log(log_path)
            log.new_lines()

            print("[-] no activity running: nothing is reconnected")
            hub.http_post("/api/ir-send", {"deviceId": "1", "command": "PowerOn"})
            check(triggered(log.new_lines()) == [], "PowerOn while off triggers nothing")

            st, r = hub.http_post("/api/activity", {"activity": "10"})
            check(st == 200, "activity 10 starts", "%s %s" % (st, r))
            for _ in range(50):
                st, a = hub.http_get("/api/activity")
                if isinstance(a, dict) and str(a.get("activity")) == "10" and not a.get("busy"):
                    break
                time.sleep(0.2)
            log.new_lines()

            print("[-] activity running")
            hub.http_post("/api/ir-send", {"deviceId": "1", "command": "PowerOn"})
            got = triggered(log.new_lines())
            check(got == ["Stream Box"], "PowerOn on the trigger device reconnects its BT device only",
                  str(got))

            hub.http_post("/api/ir-send", {"deviceId": "1", "command": "InputHdmi1"})
            got = triggered(log.new_lines())
            check(got == ["Audio Link"], "\"<deviceId>:<command>\" trigger form", str(got))

            hub.http_post("/api/ir-send", {"deviceId": "1", "command": "PowerOff"})
            check(triggered(log.new_lines()) == [], "a power-down command never reconnects")

            hub.http_post("/api/ir-send", {"deviceId": "5", "command": "Home"})
            got = triggered(log.new_lines())
            check(got == ["Stream Box"], "a command for the BT device itself reconnects it", str(got))

            hub.http_post("/api/ir-send", {"deviceId": "6", "command": "Home"})
            check(triggered(log.new_lines()) == [], "btAutoReconnect 0: never reconnected")
    finally:
        try:
            os.unlink(log_path)
        except OSError:
            pass

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] BT reconnect trigger: all checks passed")


if __name__ == "__main__":
    main()
