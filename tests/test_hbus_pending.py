#!/usr/bin/env python3
"""
Logitech protocol shim (port 8088 in the field): a runactivity request that can no longer
complete is forgotten. A request queued behind a running start and then replaced by a later
request (here a power off from the REST API) used to stay remembered, and the next time anyone
started that activity its completion frame went to the client's fd, whoever held it by then.

hub_daemon_test on port 9103, the shim on 9104, its own mock root.
"""

import json
import os
import sys
import time
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, WsClient, SAMPLE_CONFIG  # noqa: E402

PORT = 9103
LEGACY_PORT = 9104
REMOTE_ID = 1234567
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def frames(ws, seconds):
    out = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        m = ws.recv_json(timeout=max(0.05, end - time.monotonic()))
        if m is None:
            break
        out.append(m)
    return out


def wait_idle(hub, activity, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        st, r = hub.http_get("/api/activity")
        if isinstance(r, dict) and not r.get("busy") and not r.get("queued") and r.get("activity") == activity:
            return True
        time.sleep(0.1)
    return False


def main():
    with open(os.path.join(SAMPLE_CONFIG, "settings.json")) as f:
        settings = json.load(f)
    settings["legacy_enabled"] = True
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    for a in acts["activities"]:
        if a["id"] == "11":  # a start long enough to queue behind
            a.setdefault("startSequence", []).insert(0, {"type": "Delay", "delay": 1500})
    print("[-] a replaced runactivity request is forgotten")
    with HubHarness(port=PORT, args=["--legacy-port", str(LEGACY_PORT), "--ssdp-port", "0"],
                    extra_config_files={"settings.json": settings, "activities.json": acts},
                    extra_root_files={"luaworks/provision/preferences": {"remoteId": REMOTE_ID}}) as hub:
        ws = WsClient("127.0.0.1", LEGACY_PORT, path="/?domain=svcs.myharmony.com&hubId=%d" % REMOTE_ID, timeout=5.0)
        frames(ws, 0.5)
        st, r = hub.http_post("/api/activity", {"activity": "11"})
        check(isinstance(r, dict) and r.get("state") == "starting", "activity 11 starting", str(r))
        rid = str(uuid.uuid4())
        ws.send_json({"hubId": str(REMOTE_ID), "timeout": 5,
                      "hbus": {"cmd": "harmony.activityengine?runactivity", "id": rid,
                               "params": {"async": "true", "timestamp": 0, "activityId": "10"}}})
        time.sleep(0.3)
        st, r = hub.http_post("/api/activity", {"activity": "-1"})
        check(isinstance(r, dict) and r.get("state") == "queued", "power off replaces the queued 10", str(r))
        check(wait_idle(hub, "-1"), "Hub ends powered off")
        frames(ws, 0.5)
        st, r = hub.http_post("/api/activity", {"activity": "10"})
        wait_idle(hub, "10")
        got = frames(ws, 1.5)
        stale = [f for f in got if f.get("id") == rid and f.get("cmd") == "harmony.engine?startActivity"]
        check(not stale, "no frame for the replaced request at a later start of 10",
              str([(f.get("code"), f.get("id")) for f in stale]))
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] Shim pending requests: all checks passed")


if __name__ == "__main__":
    main()
