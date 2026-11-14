#!/usr/bin/env python3
"""
The "Activity switched" marker always fits in a transition. With a 64-step stop sequence and a
64-step start sequence (the most a saved sequence holds) plus a power step, the main queue
(128 steps) used to be full before the marker: it was left out, the start tail after the
sequence's Switched step (anchored to the marker) never ran, and the Hub kept reporting
"sending" until the next transition. Now the last slot is kept for the marker.
The daemon runs with --dry-run.
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8970


def config_dir():
    d = tempfile.mkdtemp(prefix="sw_marker_")
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    delay = {"type": "Delay", "delay": 1}
    acts["activities"].append({"id": "20", "name": "Long stop", "type": "Custom", "order": 3, "deviceIds": [],
                               "startSequence": [], "stopSequence": [delay] * 64})
    acts["activities"].append({"id": "21", "name": "Long start", "type": "Custom", "order": 4, "deviceIds": ["1"],
                               "startSequence": [delay] * 62 + [{"type": "Switched"},
                                                                {"type": "Command", "deviceId": "1", "command": "InputHdmi1"}],
                               "stopSequence": []})
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("devices.json", "settings.json"):
        shutil.copy(os.path.join(SAMPLE_CONFIG, name), os.path.join(d, name))
    return d


def state_of(hub):
    st, body = hub.http_get("/api/activity")
    return body if isinstance(body, dict) else {}


def wait_for(hub, pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        s = state_of(hub)
        if pred(s):
            return s
        time.sleep(0.1)
    return None


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
            st, _ = hub.http_post("/api/activity", {"activity": "20"})
            check(st == 200 and wait_for(hub, lambda s: str(s.get("activity")) == "20" and not s.get("busy"), 10) is not None,
                  "activity 20 (64-step stop sequence) running")
            st, _ = hub.http_post("/api/activity", {"activity": "21"})
            s = wait_for(hub, lambda s: str(s.get("activity")) == "21" and s.get("state") == 2 and not s.get("sending"), 30)
            check(s is not None, "21 running and nothing left to send", str(state_of(hub)))
            time.sleep(0.3)
        log = open(log_path, errors="replace").read()
        check("Start tail: InputHdmi1" in log, "the start tail after the Switched step ran")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("switched marker: %d FAILED" % len(failures))
        return 1
    print("all switched marker checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
