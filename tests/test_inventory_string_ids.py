#!/usr/bin/env python3
"""
The orchestrator's inventories keep device and activity ids as they are stored. They used to keep
only ids that parse as a number (atol): a device imported from the Harmony database (id
"d_<number>") got no automatic power on or off, and an activity with an id such as "movie"
started and powered off without sending anything. The steps come from dry-run previews and the
daemon's log; it runs with --dry-run.
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8976


def config_dir():
    d = tempfile.mkdtemp(prefix="inv_ids_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    tv = json.loads(json.dumps([x for x in devices["devices"] if str(x.get("id")) == "1"][0]))
    tv["id"], tv["name"] = "d_1731500000", "Imported TV"
    devices["devices"].append(tv)
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({"id": "15", "name": "Imported", "type": "Custom", "order": 3, "deviceIds": ["d_1731500000"],
                               "startSequence": [{"type": "Command", "deviceId": "d_1731500000", "command": "InputHdmi1"}],
                               "stopSequence": []})
    acts["activities"].append({"id": "movie", "name": "Movie", "type": "Custom", "order": 4, "deviceIds": ["1"],
                               "startSequence": [{"type": "Command", "deviceId": "1", "command": "InputHdmi1"}],
                               "stopSequence": [{"type": "Command", "deviceId": "1", "command": "VolumeDown"}]})
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    shutil.copy(os.path.join(SAMPLE_CONFIG, "settings.json"), os.path.join(d, "settings.json"))
    return d


def wait_state(hub, activity, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        st, s = hub.http_get("/api/activity")
        if isinstance(s, dict) and str(s.get("activity")) == activity and s.get("state") in (0, 2) and not s.get("busy"):
            return True
        time.sleep(0.05)
    return False


def steps(hub, path, payload):
    st, r = hub.http_post(path, payload)
    return [x.get("description") for x in (r.get("steps") or [])] if st == 200 and isinstance(r, dict) else []


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
            s = steps(hub, "/api/activity", {"activity": "15", "dry_run": True})
            check("Power on Imported TV" in s, "device d_1731500000: the start powers it on", str(s))
            s = steps(hub, "/api/activity", {"activity": "movie", "dry_run": True})
            check("Starting: InputHdmi1" in s and "Power on Living Room TV" in s, "activity 'movie': its start steps are built", str(s))
            st, _ = hub.http_post("/api/activity", {"activity": "movie"})
            check(st == 200 and wait_state(hub, "movie"), "activity 'movie' runs")
            s = steps(hub, "/api/poweroff", {"dry_run": True})
            check("Stop: VolumeDown" in s and "Power off Living Room TV" in s, "its power off sends its stop sequence and the power off", str(s))
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("inventory string ids: %d FAILED" % len(failures))
        return 1
    print("all inventory string id checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
