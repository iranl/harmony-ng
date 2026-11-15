#!/usr/bin/env python3
"""
POST /import checks every part before it writes anything: a cut devices file, or a bundle with
one bad section, is refused with 400 and the live files stay as they were. The empty list an
export writes for a file it could not read is skipped instead of written. A valid restore
still works and leaves no temporary files behind.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8905
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def device_names(hub):
    st, body = hub.http_get("/api/devices")
    devs = body.get("devices", body) if isinstance(body, dict) else body
    if not isinstance(devs, list):
        return st, None
    return st, [d.get("name") for d in devs if isinstance(d, dict)]


def main():
    print("[-] Import validates every part before writing")
    with HubHarness(port=PORT) as hub:
        cfg = os.path.join(hub.data_dir, "config")
        dev_path = os.path.join(cfg, "devices.json")
        act_path = os.path.join(cfg, "activities.json")
        set_path = os.path.join(cfg, "settings.json")
        dev_before = read(dev_path)
        act_before = read(act_path)
        st, names_before = device_names(hub)
        check(st == 200 and names_before and len(names_before) == 2, "two devices before", str(names_before))

        # 1. a cut devices file (first half of the real one)
        text = dev_before.decode()
        st, body = hub.http_post("/import", {"target": "devices", "payload": text[:len(text) // 2]})
        check(st == 400, "cut devices file: 400", "%s %s" % (st, body))
        check(read(dev_path) == dev_before, "devices.json unchanged after the cut file")
        check(device_names(hub)[1] == names_before, "the API still lists both devices")

        # 2. a bundle whose activities section is not valid: the valid devices section is not
        #    written either
        new_devs = json.loads(text)
        new_devs["devices"][0]["name"] = "Renamed TV"
        bundle = {"version": 1, "devices": new_devs, "activities": "{\"activities\": [1, 2"}
        st, body = hub.http_post("/import", {"target": "bundle", "payload": bundle})
        check(st == 400, "bundle with a cut activities section: 400", "%s %s" % (st, body))
        check(read(dev_path) == dev_before and read(act_path) == act_before,
              "no file written for the refused bundle")

        # 3. devices of the wrong shape (no devices array)
        st, body = hub.http_post("/import", {"target": "devices", "payload": {"items": []}})
        check(st == 400 and read(dev_path) == dev_before, "devices without a devices array: 400", str(st))

        # 4. the empty list an export writes for an unreadable devices file is skipped
        settings = json.loads(read(set_path))
        settings["hub_name"] = "Imported Hub"
        bundle = {"version": 1, "devices": [], "activities": [], "settings": settings}
        st, body = hub.http_post("/import", {"target": "bundle", "payload": bundle})
        check(st == 200, "bundle with empty device and activity lists: 200", "%s %s" % (st, body))
        check(read(dev_path) == dev_before and read(act_path) == act_before,
              "the empty lists did not overwrite devices.json and activities.json")
        check(json.loads(read(set_path) or b"{}").get("hub_name") == "Imported Hub", "its settings were restored")

        # 5. a valid single-target restore still works
        st, body = hub.http_post("/import", {"target": "devices", "payload": new_devs})
        check(st == 200, "valid devices restore: 200", "%s %s" % (st, body))
        check(b"Renamed TV" in (read(dev_path) or b""), "devices.json has the new content")
        check("Renamed TV" in (device_names(hub)[1] or []), "the API lists the restored device")
        leftovers = [n for n in os.listdir(cfg) if ".import" in n or ".tmp" in n]
        check(not leftovers, "no temporary files left", str(leftovers))

    if failed:
        print("[!] test_import_validate: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_import_validate: all checks passed")


if __name__ == "__main__":
    main()
