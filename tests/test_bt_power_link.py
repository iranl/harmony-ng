#!/usr/bin/env python3
"""
Bluetooth links around activity power on and off (orchestrator):
- after a Bluetooth device's power-off commands the orchestrator closes that device's host link
  (disconnect_host <addr> to hub_btstack), as the stock firmware does; also for an always-on
  Bluetooth device; never for an IR-only device
- while an activity starts, /tmp/current_activity (what hub_btstack reads before it reconnects a
  host) holds the activity being started; a power off sets it to -1 at once
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

PORT = 8983
MIRROR = "/tmp/current_activity"
STREAMER = "0A:0B:0C:0D:0E:01"  # test addresses, not a real device
SPEAKER = "0A:0B:0C:0D:0E:02"


def config_dir():
    d = tempfile.mkdtemp(prefix="bt_power_link_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devices["devices"].append({
        "id": "3", "name": "Streamer", "type": "BT", "bdaddr": STREAMER,
        "commands": [{"name": "Home", "raw": "HID:0C:0223"}, {"name": "Sleep", "raw": "HID:01:0082"}],
        "powerOnSequence": [{"type": "Command", "deviceId": "3", "command": "Home"}],
        "powerOffSequence": [{"type": "Command", "deviceId": "3", "command": "Sleep"}],
    })
    devices["devices"].append({
        "id": "4", "name": "Speaker", "type": "BT", "bdaddr": SPEAKER, "isPowerAlwaysOn": True,
        "commands": [{"name": "VolumeUp", "raw": "HID:0C:00E9"}],
    })
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({
        "id": "12", "name": "Stream", "type": "Custom", "order": 3, "deviceIds": ["1", "3", "4"],
        "startSequence": [
            {"type": "Command", "deviceId": "1", "command": "PowerOn"},
            {"type": "Command", "deviceId": "3", "command": "Home"},
            {"type": "Delay", "delay": 1500},
            {"type": "Command", "deviceId": "1", "command": "InputHdmi1"},
        ],
        "stopSequence": [],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


def read_mirror():
    try:
        with open(MIRROR) as f:
            return f.read().strip()
    except OSError:
        return None


def state_of(hub):
    st, body = hub.http_get("/api/activity")
    return body if isinstance(body, dict) else {}


def wait_state(hub, activity, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        s = state_of(hub)
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
            time.sleep(0.4)  # inside the 1.5 s delay step: still starting
            s = state_of(hub)
            check(str(s.get("activity")) == "-1" and s.get("state") == 1,
                  "the Hub's current activity is still -1 while it starts", str(s))
            check(read_mirror() == "12", "while it starts, /tmp/current_activity holds the activity being started",
                  str(read_mirror()))
            check(wait_state(hub, "12"), "activity 12 running")
            check(read_mirror() == "12", "running: /tmp/current_activity 12", str(read_mirror()))

            st, _ = hub.http_post("/api/poweroff", {})
            check(st == 200, "power off")
            check(read_mirror() == "-1", "power off: /tmp/current_activity -1 at once", str(read_mirror()))
            check(wait_state(hub, "-1"), "powered off")
            time.sleep(0.3)

        log = open(log_path, errors="replace").read().splitlines()

        def first(pred):
            return next((i for i, l in enumerate(log) if pred(l)), -1)

        sleep_i = first(lambda l: "Orch step" in l and "Power off Streamer: Sleep" in l)
        disc_streamer = first(lambda l: "disconnect_host %s" % STREAMER in l)
        disc_speaker = first(lambda l: "disconnect_host %s" % SPEAKER in l)
        check(sleep_i >= 0, "the Streamer's power-off command ran")
        check(disc_streamer > sleep_i >= 0, "its link is closed after its power-off command",
              "sleep line %d, disconnect line %d" % (sleep_i, disc_streamer))
        check(disc_speaker >= 0, "an always-on Bluetooth device's link is closed too")
        check(sum("disconnect_host" in l for l in log) == 2, "no disconnect for IR-only devices",
              str([l for l in log if "disconnect_host" in l]))
        start_i = first(lambda l: "Orch step" in l and "Home" in l)
        check(start_i >= 0 and not any("disconnect_host" in l for l in log[:start_i]),
              "nothing is disconnected while the activity starts")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("bt power link: %d FAILED" % len(failures))
        return 1
    print("all bt power link checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
