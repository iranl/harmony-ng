#!/usr/bin/env python3
"""
#260 and the Display Order of #261 against hub_daemon_test.

Saved command (Edit Saved Command dialog): both web UIs post it as a form, so port and repeats
arrive as text; they are stored, a send uses the command's own port before the device's
controlPort, and "Default (Device Setting)" (port 0) removes it. Ports that the sanitizer once
copied from the device into every command are dropped once (commandPorts 1).

Activities: the file is written back once in Display Order at load (hub_rf and MQTT read the
file), an activity without an order gets its own after the highest, and a save without "order"
keeps the stored one.
"""

import json
import os
import re
import sys
import tempfile
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import SAMPLE_CONFIG, HubHarness  # noqa: E402

PORT = 8949
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def load(path):
    with open(path) as f:
        return json.load(f)


def main():
    print("[-] Saved command port and repeats (#260), activities in Display Order")
    devices = load(os.path.join(SAMPLE_CONFIG, "devices.json"))
    devices.pop("commandPorts", None)
    dev = devices["devices"][0]
    dev["controlPort"] = 7
    cmd = dev["commands"][0]
    cmd["port"] = 7  # a copy of the device's port, as the sanitizer stamped it
    cmd["repeats"] = 1
    dev_id, cmd_name = str(dev["id"]), cmd["name"]
    code = cmd.get("raw") or cmd.get("keyCode")

    acts = load(os.path.join(SAMPLE_CONFIG, "activities.json"))
    base = acts["activities"][0]
    seeded = []
    for aid, order in (("20", 3), ("21", None), ("22", 1), ("23", 3)):
        a = json.loads(json.dumps(base))
        a["id"], a["name"] = aid, "A" + aid
        a.pop("order", None)
        if order is not None:
            a["order"] = order
        seeded.append(a)
    acts["activities"] = seeded

    log = tempfile.NamedTemporaryFile(prefix="cmdport_", suffix=".log", delete=False).name
    with HubHarness(port=PORT, log_path=log,
                    extra_config_files={"devices.json": devices, "activities.json": acts}) as hub:
        cfg = os.path.join(hub.data_dir, "config")

        # --- activities at load ---
        on_flash = load(os.path.join(cfg, "activities.json"))["activities"]
        check([a["id"] for a in on_flash] == ["22", "20", "23", "21"],
              "activities.json written back in Display Order at load", str([a["id"] for a in on_flash]))
        check(next(a for a in on_flash if a["id"] == "21").get("order") == 4,
              "an activity without an order gets one after the highest", str(on_flash))
        check(os.path.exists(os.path.join(hub.data_dir, "rf_sync_pending.flag")),
              "the write-back at load marks a push to the remotes as pending")
        st, r = hub.http_get("/api/activities")
        got = [a.get("id") for a in (r.get("activities") if isinstance(r, dict) else r) or []]
        check(got == ["22", "20", "23", "21"], "GET /api/activities in the same order", str(got))

        a23 = dict(next(a for a in on_flash if a["id"] == "23"))
        a23.pop("order")
        st, r = hub.http_post("/api/activity-save", a23)
        on_flash = load(os.path.join(cfg, "activities.json"))["activities"]
        check(st == 200 and next(a for a in on_flash if a["id"] == "23").get("order") == 3,
              "a save without order keeps the stored one", "%s %s" % (st, r))
        new = dict(a23, id="", name="New one")
        st, r = hub.http_post("/api/activity-save", new)
        on_flash = load(os.path.join(cfg, "activities.json"))["activities"]
        check(st == 200 and on_flash[-1].get("name") == "New one" and on_flash[-1].get("order") == 5,
              "a new activity without order goes last", str([(a["name"], a.get("order")) for a in on_flash]))

        # --- saved command ---
        def stored():
            st, r = hub.http_get("/api/devices")
            devs = r.get("devices") if isinstance(r, dict) else r
            d = next(x for x in devs if str(x.get("id")) == dev_id)
            return next(c for c in d["commands"] if c.get("name") == cmd_name)

        check("port" not in stored(), "a port copied from the device is dropped at load", str(stored().get("port")))

        def save_form(**fields):
            body = urllib.parse.urlencode(dict(deviceId=dev_id, name=cmd_name, oldName=cmd_name,
                                               raw=code, mode="raw", **fields)).encode()
            return hub.http_post("/ir/command", None, raw=body,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})

        def sent_mask():
            mark = os.path.getsize(log)
            hub.http_post("/api/ir-send", {"deviceId": dev_id, "command": cmd_name})
            time.sleep(0.3)
            with open(log, "rb") as f:
                f.seek(mark)
                m = re.findall(rb"port_mask 0x([0-9a-f]{2})", f.read())
            return int(m[-1], 16) if m else None

        st, r = save_form(repeats="5", port="1")
        c = stored()
        check(st == 200 and c.get("port") == 1 and c.get("repeats") == 5,
              "form post stores port and repeats", "%s %s %s" % (st, r, c))
        on_flash = load(os.path.join(cfg, "devices.json"))
        check(on_flash.get("commandPorts") == 1, "devices.json marked commandPorts 1", str(on_flash.get("commandPorts")))
        check(sent_mask() == 1, "a send uses the command's own port", "")

        st, r = save_form(repeats="5")
        check(stored().get("port") == 1, "a save without port keeps it", str(stored()))

        st, r = save_form(repeats="2", port="0")
        c = stored()
        check("port" not in c and c.get("repeats") == 2, "port 0 (device setting) removes it", str(c))
        check(sent_mask() == 7, "then a send uses the device's controlPort", "")

    try:
        os.unlink(log)
    except OSError:
        pass
    if failed:
        print("[-] FAIL: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] Saved command port, activities in Display Order: all checks passed")


if __name__ == "__main__":
    main()
