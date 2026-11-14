#!/usr/bin/env python3
"""
Device and activity saves with JSON values of the wrong type, against hub_daemon_test:
a timing field set to null, a bool, an array or an object is refused with 400 (it used to
reach atoi(NULL) and kill the daemon); a sequence step whose type, command or deviceId is
not a string no longer crashes the step sanitizer, on a save or when the config is loaded
at start. A numeric step deviceId is kept.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8961
failed = 0

TIMING_FIELDS = ("interDeviceDelay", "interKeyDelay", "holdInterKeyDelay", "powerOnDelay",
                 "btReconnectDurationSec", "btReconnectDelaySec")
BAD_VALUES = (None, True, [], {})


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def alive(hub):
    return hub.proc.poll() is None and hub.http_get("/health")[0] == 200


def load(hub, name):
    with open(os.path.join(hub.data_dir, "config", name)) as f:
        return json.load(f)


def find(items, key, value):
    for it in items:
        if str(it.get(key)) == value:
            return it
    return None


def main():
    print("[-] Device and activity saves with values of the wrong type")
    with HubHarness(port=PORT) as hub:
        for field in TIMING_FIELDS:
            for bad in BAD_VALUES:
                st, r = hub.http_post("/api/device-save", {"deviceId": "1", field: bad})
                check(st == 400 and alive(hub), "%s=%s refused, daemon alive" % (field, json.dumps(bad)),
                      "%s %s" % (st, r))
                if not alive(hub):
                    return
        st, r = hub.http_post("/ir/device", {"name": "New TV", "interDeviceDelay": None})
        check(st == 400 and alive(hub), "new device with a null timing field refused", "%s %s" % (st, r))

        st, r = hub.http_post("/api/device-save", {"deviceId": "1", "interKeyDelay": 250, "powerOnDelay": "1500"})
        dev = find(load(hub, "devices.json").get("devices", []), "id", "1") or {}
        check(st == 200 and dev.get("interKeyDelay") == 250 and dev.get("powerOnDelay") == 1500,
              "number and numeric string still saved",
              "%s %s %s" % (st, dev.get("interKeyDelay"), dev.get("powerOnDelay")))

        steps = [{"command": 2}, {"type": None, "command": "PowerOff", "deviceId": "1"},
                 {"type": "Command", "command": None}, {"type": [], "command": {}},
                 {"type": "Command", "command": "PowerOn", "deviceId": 2}]
        st, r = hub.http_post("/api/device-save", {"deviceId": "1", "powerOnSequence": steps})
        check(st == 200 and alive(hub), "device power sequence with non-string fields saved", "%s %s" % (st, r))

        st, r = hub.http_post("/api/activity-save", {"id": "10", "name": "Watch TV", "startSequence": steps,
                                                     "stopSequence": [{"type": None}]})
        check(st == 200 and alive(hub), "activity sequences with non-string fields saved", "%s %s" % (st, r))
        act = find(load(hub, "activities.json").get("activities", []), "id", "10") or {}
        seq = act.get("startSequence", [])
        check([s.get("command") for s in seq] == ["PowerOff", "PowerOn"], "steps without a string command dropped",
              json.dumps(seq))
        check(len(seq) == 2 and seq[1].get("deviceId") == "2", "numeric step deviceId kept", json.dumps(seq))

    # A config file with such a step (hand edit, older tool, backup) must not stop the daemon at start
    with open(os.path.join(SAMPLE_CONFIG, "activities.json")) as f:
        acts = json.load(f)
    acts["activities"][0]["startSequence"] = [{"command": 2}, {"type": None}]
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        devs = json.load(f)
    devs["devices"][0]["powerOnSequence"] = [{"command": None}]
    try:
        with HubHarness(port=PORT, extra_config_files={"activities.json": acts, "devices.json": devs}) as hub:
            check(alive(hub), "daemon starts with non-string step fields in the config")
    except RuntimeError as e:
        check(False, "daemon starts with non-string step fields in the config", str(e))

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] All wrong-type save checks passed")


if __name__ == "__main__":
    main()
