#!/usr/bin/env python3
"""
Device, command and activity names never carry '<', '>' or control characters: the sanitizer
(config_sanitize.c) drops them when a config is loaded and when it is saved, so a name can
never become markup in a client that does not escape it (Home Assistant cards, other apps).
"""

import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8944
failed = 0
EVIL = "<img src=x onerror=alert(1)>Pwn"
EVIL_CLEAN = "img src=x onerror=alert(1)Pwn"


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def main():
    print("[-] names lose '<', '>' and control characters")
    cfg = tempfile.mkdtemp(prefix="name_sanitize_")
    for f in os.listdir(SAMPLE_CONFIG):
        shutil.copy(os.path.join(SAMPLE_CONFIG, f), cfg)
    with open(os.path.join(cfg, "devices.json")) as f:
        devs = json.load(f)
    devs["devices"][0]["name"] = EVIL
    devs["devices"][0]["manufacturer"] = "Acme\r\n<b>"
    devs["devices"][0]["commands"][0]["name"] = "Power<script>"
    with open(os.path.join(cfg, "devices.json"), "w") as f:
        json.dump(devs, f)
    with open(os.path.join(cfg, "activities.json")) as f:
        acts = json.load(f)
    acts["activities"][0]["name"] = "Watch <svg onload=1> TV"
    with open(os.path.join(cfg, "activities.json"), "w") as f:
        json.dump(acts, f)

    with HubHarness(port=PORT, config_dir=cfg) as hub:
        st, inv = hub.http_get("/api/inventory")
        d0 = inv["devices"][0] if isinstance(inv, dict) and inv.get("devices") else {}
        check(d0.get("name") == EVIL_CLEAN, "device name cleaned at load", str(d0.get("name")))
        names = [c.get("name") if isinstance(c, dict) else c for c in d0.get("commands", [])]
        check("Powerscript" in names and not any("<" in (n or "") for n in names), "command name cleaned at load", str(names[:6]))
        st, acts_r = hub.http_get("/api/activities")
        alist = acts_r.get("activities", acts_r) if isinstance(acts_r, dict) else acts_r
        anames = [a.get("name") for a in alist] if isinstance(alist, list) else []
        check("Watch svg onload=1 TV" in anames, "activity name cleaned at load", str(anames))

        st, r = hub.http_post("/api/commands-batch", {"deviceId": d0.get("id", "1"),
                                                      "commands": [{"name": "New<img src=x onerror=1>", "raw": "X"}]})
        check(st == 200, "commands-batch accepted", "%s %s" % (st, r))
        with open(os.path.join(hub.data_dir, "config", "devices.json")) as f:
            saved = json.load(f)
        text = json.dumps(saved)
        check("<" not in text and ">" not in text, "no '<' or '>' anywhere in the saved devices.json")
        snames = [c.get("name") for c in saved["devices"][0].get("commands", [])]
        check("Newimg src=x onerror=1" in snames, "batch command name saved cleaned", str(snames))
        check(saved["devices"][0].get("manufacturer") == "Acmeb", "control characters and markup signs dropped",
              repr(saved["devices"][0].get("manufacturer")))
    shutil.rmtree(cfg, ignore_errors=True)
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] all checks passed")


if __name__ == "__main__":
    main()
