#!/usr/bin/env python3
"""
A configured PowerOff activity ("-1", with devices or steps of its own, as a migrated stock
configuration has) does not change the automatic power steps: PowerOff means no device is on.
- from PowerOff, a start powers on the activity's devices; nothing else is powered off
- a power off powers off the activity's devices; nothing is powered on
Before, the devices of the "-1" entry counted as on: a start skipped their power on and switched
off the others it listed, a power off switched those on and left the shared ones on.
The steps come from dry-run previews; the daemon runs with --dry-run.
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8977


def config_dir(with_poweroff_entry):
    d = tempfile.mkdtemp(prefix="poweroff_act_")
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    # activity 16 uses the TV only and has no power command in its sequences
    acts["activities"].append({"id": "16", "name": "TV input", "type": "Custom", "order": 3, "deviceIds": ["1"],
                               "startSequence": [{"type": "Command", "deviceId": "1", "command": "InputHdmi1"}],
                               "stopSequence": []})
    if with_poweroff_entry:
        acts["activities"].append({"id": "-1", "name": "PowerOff", "type": "PowerOff", "order": 0, "deviceIds": ["1", "2"],
                                   "startSequence": [], "stopSequence": []})
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("devices.json", "settings.json"):
        shutil.copy(os.path.join(SAMPLE_CONFIG, name), os.path.join(d, name))
    return d


def preview(hub, path, payload):
    st, r = hub.http_post(path, payload)
    return [s.get("description") for s in (r.get("steps") or [])] if st == 200 and isinstance(r, dict) else ["HTTP %s %s" % (st, r)]


def wait_state(hub, activity, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        st, s = hub.http_get("/api/activity")
        if isinstance(s, dict) and str(s.get("activity")) == activity and s.get("state") in (0, 2) and not s.get("busy"):
            return True
        time.sleep(0.05)
    return False


def run(with_entry):
    cfg = config_dir(with_entry)
    try:
        with HubHarness(port=PORT, config_dir=cfg) as hub:
            start = preview(hub, "/api/activity", {"activity": "16", "dry_run": True})
            st, _ = hub.http_post("/api/activity", {"activity": "16"})
            ok = st == 200 and wait_state(hub, "16")
            off = preview(hub, "/api/poweroff", {"dry_run": True})
            return start, off, ok
    finally:
        shutil.rmtree(cfg, ignore_errors=True)


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    ctl_start, ctl_off, ok = run(False)
    check(ok and "Power on Living Room TV" in ctl_start, "control: from PowerOff, 16 powers on the TV", str(ctl_start))
    check("Power off Living Room TV" in ctl_off, "control: the power off powers off the TV", str(ctl_off))

    start, off, ok = run(True)
    check(ok, "16 runs with a PowerOff entry configured")
    check(start == ctl_start, "with a PowerOff entry listing TV and Media Player: the same start steps", "%s vs %s" % (start, ctl_start))
    check(not any("Media Player" in s for s in start), "the start does not touch the Media Player", str(start))
    check(off == ctl_off, "the same power off steps", "%s vs %s" % (off, ctl_off))
    check(not any(s.startswith("Power on") for s in off), "the power off powers nothing on", str(off))

    if failures:
        print("poweroff activity: %d FAILED" % len(failures))
        return 1
    print("all poweroff activity checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
