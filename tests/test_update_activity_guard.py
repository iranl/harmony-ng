#!/usr/bin/env python3
"""
A software update must not reboot the Hub in the middle of an activity switch (as agreed in #220):
- POST /api/update-apply while a switch runs answers 409 with status "activity_switch" and the
  orchestrator fields (busy, sending, queued), and starts nothing
- the same over the WebSocket command channel (update_apply): an ack with code 409, the error and
  the status, busy, sending, queued fields
- a request queued behind the running switch keeps the refusal
- once the switch has completed, the update starts (200, status "applying"; the test build never
  runs the updater)
The daemon runs with --dry-run on a private mock root (port 9200); the start sequence of the test
activity has a 2.5 s delay.
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 9200
failures = []


def check(cond, name, detail=""):
    print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
    if not cond:
        failures.append(name)


def config_dir():
    d = tempfile.mkdtemp(prefix="upd_guard_")
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({
        "id": "12", "name": "Slow", "type": "Custom", "order": 3,
        "startSequence": [
            {"type": "Command", "deviceId": "1", "command": "PowerOn"},
            {"type": "Delay", "delay": 2500},
            {"type": "Command", "deviceId": "1", "command": "InputHdmi1"},
        ],
        "stopSequence": [{"type": "Command", "deviceId": "1", "command": "PowerOff"}],
    })
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("devices.json", "settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


def wait_idle(hub, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        st, body = hub.http_get("/api/activity")
        if isinstance(body, dict) and not body.get("busy") and not body.get("sending") and body.get("queued") is None:
            return body
        time.sleep(0.1)
    return None


def ws_until(ws, pred, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            m = ws.recv_json(timeout=max(0.1, deadline - time.time()))
        except Exception:
            continue
        if m and pred(m):
            return m
    return None


def main():
    cfg = config_dir()
    log_path = os.path.join(cfg, "hub.log")
    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log_path) as hub:
            print("[-] update while idle is allowed (baseline)")
            check(wait_idle(hub) is not None, "idle at start")

            print("[-] update during an activity switch")
            st, r = hub.http_post("/api/activity", {"activity": "12"})
            check(st == 200, "start of the slow activity accepted", "%s %s" % (st, r))
            time.sleep(0.3)
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 409, "POST /api/update-apply: 409", "%s %s" % (st, r))
            check(isinstance(r, dict) and r.get("ok") is False and r.get("status") == "activity_switch"
                  and r.get("busy") is True and "error" in r, "reply names the activity switch", str(r))

            ws = hub.ws()
            ws.recv_json(timeout=5)  # snapshot
            ws.send_json({"action": "update_apply", "msgId": 51})
            ack = ws_until(ws, lambda m: m.get("msgId") == 51, 3)
            check(ack is not None and ack.get("ok") is False and ack.get("code") == 409
                  and "activity switch" in (ack.get("error") or ""), "command channel update_apply: 409", str(ack))
            check(ack is not None and ack.get("status") == "activity_switch" and ack.get("busy") is True
                  and "sending" in ack and "queued" in ack, "the ack carries status, busy, sending, queued", str(ack))

            st, r = hub.http_post("/api/poweroff", {})
            check(st == 200 and isinstance(r, dict) and r.get("state") == "queued", "power off queued", str(r))
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 409 and isinstance(r, dict) and r.get("queued") == "-1",
                  "refused while a request is queued, queued id in the reply", "%s %s" % (st, r))

            print("[-] after the switch")
            check(wait_idle(hub) is not None, "switch and queued power off completed")
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and isinstance(r, dict) and r.get("status") == "applying", "update starts once idle",
                  "%s %s" % (st, r))
            ws.send_json({"action": "update_apply", "msgId": 52})
            ack = ws_until(ws, lambda m: m.get("msgId") == 52, 3)
            check(ack is not None and ack.get("ok") is True, "command channel update_apply once idle", str(ack))
            ws.close()
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("FAILED: %d check(s)" % len(failures))
        sys.exit(1)
    print("All update guard checks passed")


if __name__ == "__main__":
    main()
