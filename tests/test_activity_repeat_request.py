#!/usr/bin/env python3
"""
A request for the activity the Hub is already in, while no transition runs, changes nothing.
It used to clear the background queue: a second press right after the Switched marker dropped
the tail of the sequence that had just run.
- start 12, ask for 12 again as soon as it reports running: the start tail (600 ms after the
  marker) is still sent
- power off, power off again as soon as it reports idle: the stop tail (400 ms after the marker)
  is still sent
- the repeated request still emits the events of an empty transition (starting, then running),
  which the Logitech shim answers a runactivity on
The daemon runs with --dry-run; sends are read from its log.
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8975


def config_dir():
    d = tempfile.mkdtemp(prefix="act_repeat_")
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({
        "id": "12", "name": "SwitchedAct", "type": "Custom", "order": 3, "deviceIds": ["1"],
        "startSequence": [
            {"type": "Command", "deviceId": "1", "command": "PowerOn"},
            {"type": "Switched"},
            {"type": "Delay", "delay": 600},
            {"type": "Command", "deviceId": "1", "command": "InputHdmi1"},
        ],
        "stopSequence": [
            {"type": "Command", "deviceId": "1", "command": "PowerOff"},
            {"type": "Switched"},
            {"type": "Delay", "delay": 400},
            {"type": "Command", "deviceId": "1", "command": "VolumeDown"},
        ],
    })
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("devices.json", "settings.json"):
        shutil.copy(os.path.join(SAMPLE_CONFIG, name), os.path.join(d, name))
    return d


def wait_state(hub, activity, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        st, s = hub.http_get("/api/activity")
        if isinstance(s, dict) and str(s.get("activity")) == activity and s.get("state") in (0, 2) and not s.get("busy"):
            return True
        time.sleep(0.02)
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
            check(st == 200 and wait_state(hub, "12"), "12 reports running (Switched marker)")
            ws = hub.ws()
            st, r = hub.http_post("/api/activity", {"activity": "12"})
            check(st == 200, "asking for 12 again is accepted", "%s %s" % (st, r))
            m, frames = ws.wait_for("activity_state", 3, lambda m: m.get("state") == 2 and m.get("activity") == "12")
            check(m is not None and any(f.get("type") == "activity_state" and f.get("state") == 1 for f in frames),
                  "it still reports an empty transition (starting, then running)", str(frames))
            time.sleep(1.2)
            log = open(log_path, errors="replace").read()
            check("Start tail: InputHdmi1" in log, "the start tail is still sent")

            st, _ = hub.http_post("/api/poweroff", {})
            check(st == 200 and wait_state(hub, "-1"), "power off reports idle (Switched marker)")
            st, r = hub.http_post("/api/poweroff", {})
            check(st == 200, "a second power off is accepted", "%s %s" % (st, r))
            time.sleep(1.0)
            log = open(log_path, errors="replace").read()
            check("Stop tail: VolumeDown" in log, "the stop tail is still sent")
            check(log.count("stop_reconnect") <= 1 if "stop_reconnect" in log else True, "one stop_reconnect for one power off")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("activity repeat request: %d FAILED" % len(failures))
        return 1
    print("all activity repeat request checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
