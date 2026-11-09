#!/usr/bin/env python3
"""
Activity requests while a transition runs are queued, not refused (orchestrator):
- a power off asked during a start is accepted (200, state "queued"), GET /api/activity shows it,
  and it runs when the start has completed: the activity reaches running, then idle
- a later request replaces the queued one (start 10, then 11 queued, then power off queued: the Hub
  ends idle and activity 11 never starts)
- a request for the transition's own target is not queued; an unknown activity is a 500, not queued
- the WebSocket command channel answers ok with state "queued" (and names the action it was called by)
The daemon runs with --dry-run; the start sequence of the test activity has a 1.5 s delay.
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8982


def config_dir():
    d = tempfile.mkdtemp(prefix="act_queue_")
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({
        "id": "12", "name": "Slow", "type": "Custom", "order": 3,
        "startSequence": [
            {"type": "Command", "deviceId": "1", "command": "PowerOn"},
            {"type": "Delay", "delay": 1500},
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


def state_of(hub):
    st, body = hub.http_get("/api/activity")
    return body if isinstance(body, dict) else {}


def wait_for(hub, pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        s = state_of(hub)
        if pred(s):
            return s
        time.sleep(0.1)
    return None


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    cfg = config_dir()
    log_path = os.path.join(cfg, "hub.log")
    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log_path) as hub:
            print("[-] power off during a start")
            st, r = hub.http_post("/api/activity", {"activity": "12"})
            check(st == 200 and r.get("state") == "starting", "start 12 accepted", "%s %s" % (st, r))
            time.sleep(0.3)
            st, r = hub.http_post("/api/poweroff", {})
            check(st == 200 and r.get("state") == "queued", "power off during the start: 200, queued", "%s %s" % (st, r))
            s = state_of(hub)
            check(s.get("busy") is True and s.get("queued") == "-1", "GET /api/activity: busy, queued -1", str(s))
            s = wait_for(hub, lambda s: str(s.get("activity")) == "-1" and s.get("state") == 0 and not s.get("busy"))
            check(s is not None and s.get("queued") is None, "the queued power off ran: idle, queue empty", str(state_of(hub)))
            # the queued power off starts the instant the start completes, so "12 running" is not
            # observable over REST: the order of the steps in the log shows the start finished first
            log = open(log_path, errors="replace").read()
            i_last_start = log.find("Starting: InputHdmi1")
            i_off = log.find("Stop: PowerOff")  # activity 12 stop sequence
            check(0 <= i_last_start < i_off, "the start completed first, then the power off (log order)",
                  "start step at %d, power off at %d" % (i_last_start, i_off))

            print("[-] a later request replaces the queued one")
            st, r = hub.http_post("/api/activity", {"activity": "12"})
            time.sleep(0.2)
            st, r = hub.http_post("/api/activity", {"activity": "10"})
            check(st == 200 and r.get("state") == "queued", "start 10 during the start of 12: queued")
            st, r = hub.http_post("/api/poweroff", {})
            check(st == 200 and r.get("state") == "queued" and state_of(hub).get("queued") == "-1",
                  "power off replaces the queued 10", str(state_of(hub)))
            st, r = hub.http_post("/api/activity", {"activity": "999"})
            check(st == 500 and state_of(hub).get("queued") == "-1",
                  "an unknown activity during the start: 500, the queue keeps the power off", "%s %s %s" % (st, r, state_of(hub)))
            st, r = hub.http_post("/api/activity", {"activity": "12"})
            check(st == 200 and r.get("state") == "starting" and state_of(hub).get("queued") is None,
                  "asking for the transition's own target drops the queue", "%s %s %s" % (st, r, state_of(hub)))
            s = wait_for(hub, lambda s: str(s.get("activity")) == "12" and s.get("state") == 2 and not s.get("busy"))
            check(s is not None, "12 running, nothing queued", str(state_of(hub)))
            time.sleep(0.5)
            check(str(state_of(hub).get("activity")) == "12", "and it stays running (no stale request)")

            print("[-] WebSocket command channel")
            ws = hub.ws()
            ws.recv_json(5)  # snapshot
            ws.send_json({"action": "power_off", "msgId": 7})
            m, _ = ws.wait_for("ack", 5)
            check(m is not None and m.get("ok") is True and m.get("state") == "stopping" and m.get("action") == "power_off",
                  "power_off while 10 runs: stopping, ack names the action", str(m))
            time.sleep(0.2)
            ws.send_json({"action": "start_activity", "activity": "10", "msgId": 8})
            m, _ = ws.wait_for("ack", 5)
            check(m is not None and m.get("ok") is True and m.get("state") == "queued", "start_activity during the power off: ok, queued", str(m))
            s = wait_for(hub, lambda s: str(s.get("activity")) == "10" and s.get("state") == 2 and not s.get("busy"))
            check(s is not None, "the queued start runs after the power off (10 running)", str(state_of(hub)))
            ws.close()

        log = open(log_path, errors="replace").read()
        check("requested while switching to" in log and "starting the queued request" in log, "the queue is logged")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("activity queue: %d FAILED" % len(failures))
        return 1
    print("all activity queue checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
