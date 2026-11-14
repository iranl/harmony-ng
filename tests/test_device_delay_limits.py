#!/usr/bin/env python3
"""
The per-device send delays (interDeviceDelay, interKeyDelay, holdInterKeyDelay) are slept out by
the command sender before the next command, on the caller's thread (the hub_daemon main loop for
WebSocket, MQTT and activity steps). They had no upper limit: a device saved with 600000 blocked
the daemon for 10 minutes on its next command. Against hub_daemon_test:

  - a save with a delay above 10000 ms or below 0 is refused with 400 and changes nothing;
  - 0 and 10000 (and a numeric string in range) are still saved;
  - a devices.json that already holds a larger value is lowered to 10000 when it is loaded.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8942
FIELDS = ("interDeviceDelay", "interKeyDelay", "holdInterKeyDelay")
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def device(hub, dev_id):
    with open(os.path.join(hub.data_dir, "config", "devices.json")) as f:
        for d in json.load(f).get("devices", []):
            if str(d.get("id")) == dev_id:
                return d
    return {}


def main():
    print("[-] Device send delays limited to 0..10000 ms")
    with HubHarness(port=PORT) as hub:
        before = device(hub, "1")
        for field in FIELDS:
            for bad in (10001, 600000, "600000", -1, -5000):
                st, r = hub.http_post("/api/device-save", {"deviceId": "1", field: bad})
                after = device(hub, "1")
                check(st == 400 and after.get(field) == before.get(field),
                      "%s=%s refused, nothing changed" % (field, json.dumps(bad)),
                      "%s %s stored %s" % (st, r, after.get(field)))
        st, r = hub.http_post("/api/device-save", {"deviceId": "1", "name": "Renamed", "interKeyDelay": 99999})
        check(st == 400 and device(hub, "1").get("name") == before.get("name"),
              "the whole save is refused, not only the delay", "%s %s" % (st, r))

        for field in FIELDS:
            for good, want in ((0, 0), (10000, 10000), ("250", 250)):
                st, r = hub.http_post("/api/device-save", {"deviceId": "1", field: good})
                got = device(hub, "1").get(field)
                check(st == 200 and got == want, "%s=%s saved" % (field, json.dumps(good)), "%s %s stored %s" % (st, r, got))

    # A file written before the limit (or by hand) is lowered at load
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        devs = json.load(f)
    for d in devs["devices"]:
        if str(d.get("id")) == "2":
            d.update({"interDeviceDelay": 600000, "interKeyDelay": 20000, "holdInterKeyDelay": 10000})
    with HubHarness(port=PORT, extra_config_files={"devices.json": devs}) as hub:
        st, body = hub.http_get("/api/devices")
        d2 = [d for d in (body.get("devices", []) if isinstance(body, dict) else []) if str(d.get("id")) == "2"]
        d2 = d2[0] if d2 else {}
        check(st == 200 and d2.get("interDeviceDelay") == 10000 and d2.get("interKeyDelay") == 10000
              and d2.get("holdInterKeyDelay") == 10000, "values above 10000 in devices.json loaded as 10000",
              "%s %s" % (st, {k: d2.get(k) for k in FIELDS}))

    if failed:
        print("[-] %d check(s) failed" % failed)
        return 1
    print("[+] device delay limits: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
