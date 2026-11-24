#!/usr/bin/env python3
"""POST /api/device-save keeps the command ranges of a plugin device (post-merge check of #253,
finding 6): a command of the request without a "range" key keeps the range the stored command
had; "range": null or [] removes it, and a removed range stays removed at the next save; a
command the device did not have gets the plugin manifest's range. Before the fix, a client that
sent the commands back as name and keyCode removed every range, and the Hub then sent any
value."""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9641
failed = 0
USERS = {"auth_enabled": True, "users": [{"username": "admin", "password": "secret123", "role": "admin"}]}
MAN = {"id": "ranger", "name": "Ranger", "version": "1.0.0", "entrypoint": "bin/drv",
       "device_definition": {"display_name": "Ranger", "config_fields": [{"key": "mac", "type": "string"}],
                             "default_commands": [
                                 {"name": "Level", "keyCode": "PLUGIN:ranger:Level", "range": [1, 100]},
                                 {"name": "Speed", "keyCode": "PLUGIN:ranger:Speed", "range": [0, 31]},
                                 {"name": "Home", "keyCode": "PLUGIN:ranger:Home"}]}}


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def install(hub):
    d = os.path.join(hub.data_dir, "plugins", "ranger")
    os.makedirs(os.path.join(d, "bin"), exist_ok=True)
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(MAN, f)
    with open(os.path.join(d, "bin", "drv"), "w") as f:
        f.write("#!/bin/sh\nexec sleep 300\n")
    os.chmod(os.path.join(d, "bin", "drv"), 0o755)


def ranges(hub, did):
    """{command name: range or None} of device did, from the inventory"""
    st, inv = hub.http_get("/api/inventory")
    for d in inv.get("devices", []) if isinstance(inv, dict) else []:
        if str(d.get("id")) == str(did):
            return {c.get("name"): c.get("range") for c in d.get("commands", []) if isinstance(c, dict)}
    return None


def save(hub, body):
    st, res = hub.http_post("/api/device-save", body)
    return st, res


def main():
    print("[-] Device save keeps command ranges")
    full = MAN["device_definition"]["default_commands"]
    bare = [{"name": c["name"], "keyCode": c["keyCode"], "raw": c["keyCode"]} for c in full]
    with HubHarness(port=PORT, args=["--dry-run"], extra_config_files={"users.json": USERS}) as hub:
        install(hub)
        base = {"type": "plugin:ranger", "plugin": "ranger", "config": {"mac": ""}}

        st, res = save(hub, dict(base, name="R1", commands=full))
        did = res.get("deviceId") if isinstance(res, dict) else None
        check(st == 200 and did, "new device saved", str((st, res)))
        check(ranges(hub, did) == {"Level": [1, 100], "Speed": [0, 31], "Home": None},
              "new device: commands with their ranges", str(ranges(hub, did)))

        # an API client edits the device and sends its commands back without "range"
        st, res = save(hub, dict(base, deviceId=did, id=did, name="R1", config={"mac": "AA"}, commands=bare))
        got = ranges(hub, did)
        check(st == 200 and got == {"Level": [1, 100], "Speed": [0, 31], "Home": None},
              "commands sent without \"range\" keep the stored ranges", str((st, got)))
        st, res = hub.http_post("/api/ir-send", {"deviceId": did, "command": "Level", "value": 5000})
        check(st == 400, "a value outside the kept range is refused", str((st, res)))
        st, res = hub.http_post("/api/ir-send", {"deviceId": did, "command": "Level"})
        check(st == 400, "the kept range still needs a value", str((st, res)))
        st, res = hub.http_post("/api/ir-send", {"deviceId": did, "command": "Level", "value": 50})
        check(st == 200, "a value inside the kept range is sent", str((st, res)))

        # "commands" as a JSON string (batch form) without ranges: kept too
        st, res = save(hub, dict(base, deviceId=did, id=did, name="R1", commands=json.dumps(bare)))
        got = ranges(hub, did)
        check(st == 200 and got == {"Level": [1, 100], "Speed": [0, 31], "Home": None},
              "commands as a JSON string keep the stored ranges", str((st, got)))

        # an own range is kept as well, not replaced by the manifest's
        own = [dict(bare[0], range=[5, 10]), bare[1], bare[2]]
        save(hub, dict(base, deviceId=did, id=did, name="R1", commands=own))
        save(hub, dict(base, deviceId=did, id=did, name="R1", commands=bare))
        got = ranges(hub, did)
        check(got == {"Level": [5, 10], "Speed": [0, 31], "Home": None}, "an own range is kept", str(got))

        # null and [] remove a range; a later save without "range" does not bring it back
        rm = [dict(bare[0], range=None), dict(bare[1], range=[]), bare[2]]
        st, res = save(hub, dict(base, deviceId=did, id=did, name="R1", commands=rm))
        got = ranges(hub, did)
        check(st == 200 and got == {"Level": None, "Speed": None, "Home": None},
              "range null and [] remove the range", str((st, got)))
        st, res = save(hub, dict(base, deviceId=did, id=did, name="R1", commands=bare))
        got = ranges(hub, did)
        check(got == {"Level": None, "Speed": None, "Home": None}, "a removed range stays removed", str(got))

        # a command the device did not have takes the manifest's range
        st, res = save(hub, dict(base, deviceId=did, id=did, name="R1", commands=bare[2:]))
        st, res = save(hub, dict(base, deviceId=did, id=did, name="R1", commands=bare))
        got = ranges(hub, did)
        check(got == {"Level": [1, 100], "Speed": [0, 31], "Home": None},
              "a command new to the device gets the manifest range", str(got))

        # a save without "commands" leaves them as they are
        st, res = save(hub, dict(base, deviceId=did, id=did, name="R1b", config={"mac": "BB"}))
        got = ranges(hub, did)
        check(got == {"Level": [1, 100], "Speed": [0, 31], "Home": None}, "a save without commands keeps the ranges", str(got))

    if failed:
        print("[-] test_device_save_ranges: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_device_save_ranges: all checks passed")


if __name__ == "__main__":
    main()
