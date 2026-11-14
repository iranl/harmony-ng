#!/usr/bin/env python3
"""
Bluetooth connect and disconnect requests from hub_daemon must use the commands hub_btstack
knows (connect_host / disconnect_host [<addr>]):
- an activity step of type BTCommand with CONNECT or DISCONNECT
- a device command Connect / Disconnect (or BT_CONNECT / BT_DISCONNECT) on a Bluetooth device
hub_daemon sent CONNECT and DISCONNECT, which hub_btstack drops on its command socket and, on
the key FIFO, sends as consumer usages 0x0C and 0x0D.
The daemon runs with --dry-run, so hub_btstack commands are printed, not sent.
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8986
STREAMER = "0A:0B:0C:0D:0E:01"  # test address, not a real device


def config_dir():
    d = tempfile.mkdtemp(prefix="bt_connect_cmds_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devices["devices"].append({
        "id": "3", "name": "Streamer", "type": "BT", "bdaddr": STREAMER,
        "commands": [{"name": "Home", "raw": "HID:0C:0223"}, {"name": "Connect", "raw": "Connect"},
                     {"name": "Disconnect", "raw": "Disconnect"}],
    })
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({
        "id": "12", "name": "Stream", "type": "Custom", "order": 3, "deviceIds": ["1"],
        "startSequence": [
            {"type": "Command", "deviceId": "1", "command": "PowerOn"},
            {"type": "BTCommand", "command": "CONNECT"},
        ],
        "stopSequence": [{"type": "BTCommand", "command": "DISCONNECT"}],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


def wait_state(hub, activity, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        st, body = hub.http_get("/api/activity")
        s = body if isinstance(body, dict) else {}
        if str(s.get("activity")) == activity and s.get("state") in (0, 2) and not s.get("busy"):
            return True
        time.sleep(0.1)
    return False


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    cfg = config_dir()
    log_path = os.path.join(cfg, "hub.log")
    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log_path) as hub:
            st, _ = hub.http_post("/api/activity", {"activity": "12"})
            check(st == 200, "start activity 12")
            check(wait_state(hub, "12"), "activity 12 running")
            st, _ = hub.http_post("/api/poweroff", {})
            check(st == 200, "power off")
            check(wait_state(hub, "-1"), "powered off")
            mark = len(open(log_path, errors="replace").read().splitlines())

            st, _ = hub.http_post("/api/ir-send", {"deviceId": "3", "command": "Connect"})
            check(st == 200, "device command Connect on the Bluetooth device", str(st))
            st, _ = hub.http_post("/api/ir-send", {"deviceId": "3", "command": "Disconnect"})
            check(st == 200, "device command Disconnect on the Bluetooth device", str(st))
            st, _ = hub.http_post("/api/ir-send", {"deviceId": "3", "command": "Connect", "action": "release"})
            check(st == 200, "release of a Connect key", str(st))
            time.sleep(0.3)

        log = open(log_path, errors="replace").read().splitlines()
        sent = [l.split("BTstack command simulated: ", 1)[1].strip() for l in log if "BTstack command simulated: " in l]
        acts_sent = [l.split("BTstack command simulated: ", 1)[1].strip() for l in log[:mark] if "BTstack command simulated: " in l]
        dev_sent = [l.split("BTstack command simulated: ", 1)[1].strip() for l in log[mark:] if "BTstack command simulated: " in l]

        check("connect_host" in acts_sent, "BTCommand CONNECT step: connect_host", str(acts_sent))
        check("disconnect_host" in acts_sent, "BTCommand DISCONNECT step: disconnect_host", str(acts_sent))
        check("connect_host %s" % STREAMER in dev_sent, "device command Connect: connect_host <its host>", str(dev_sent))
        check("disconnect_host %s" % STREAMER in dev_sent, "device command Disconnect: disconnect_host <its host>", str(dev_sent))
        check(dev_sent.count("connect_host %s" % STREAMER) == 1, "the release of the Connect key sends nothing", str(dev_sent))
        check(not any(c.split(" ", 1)[0] in ("CONNECT", "DISCONNECT") for c in sent),
              "no CONNECT / DISCONNECT, which hub_btstack does not know", str(sent))
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("bt connect commands: %d FAILED" % len(failures))
        return 1
    print("all bt connect command checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
