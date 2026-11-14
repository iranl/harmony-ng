#!/usr/bin/env python3
"""
A devices.json or activities.json that is there but cannot be parsed (a save cut by a full flash)
loads as an empty list, as before, but the file is moved aside as <name>.corrupt-<time> first:
the next save used to print the empty list over it, so nothing was left to repair by hand.
A missing file (first start) is not an error and leaves nothing behind.
The daemon runs with --dry-run on a mock root.
"""

import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8978
CUT_DEVICES = '{"devices": [{"id": "1", "name": "Living Room TV", "commands": [{"name": "PowerOn", "keyCo'


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    with HubHarness(port=PORT, extra_config_files={"devices.json": CUT_DEVICES}) as hub:
        cfg = os.path.join(hub.data_dir, "config")
        st, devs = hub.http_get("/api/devices")
        n = len(devs) if isinstance(devs, list) else len((devs or {}).get("devices", [])) if isinstance(devs, dict) else -1
        check(st == 200 and n == 0, "a cut devices.json loads as no devices", "%s %s" % (st, str(devs)[:100]))
        kept = glob.glob(os.path.join(cfg, "devices.json.corrupt-*"))
        check(len(kept) == 1 and open(kept[0]).read() == CUT_DEVICES, "the cut file is kept aside unchanged", str(kept))
        st, r = hub.http_post("/api/device-save", {"name": "New device", "type": "Television"})
        check(st == 200, "a device can be added", "%s %s" % (st, r))
        saved = json.load(open(os.path.join(cfg, "devices.json")))
        check([d.get("name") for d in saved.get("devices", [])] == ["New device"], "devices.json holds the new device", str(saved)[:100])
        check(len(kept) == 1 and open(kept[0]).read() == CUT_DEVICES, "the kept copy survives the save")
        check(not glob.glob(os.path.join(cfg, "activities.json.corrupt-*")), "a readable activities.json is left alone")

    with HubHarness(port=PORT, config_dir=os.path.join(os.path.dirname(os.path.abspath(__file__)), "no_such_dir")) as hub:
        cfg = os.path.join(hub.data_dir, "config")
        check(not glob.glob(os.path.join(cfg, "*.corrupt-*")), "missing files (first start) leave nothing aside", str(os.listdir(cfg)))

    if failures:
        print("config unreadable: %d FAILED" % len(failures))
        return 1
    print("all config unreadable checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
