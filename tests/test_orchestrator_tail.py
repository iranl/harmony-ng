#!/usr/bin/env python3
"""
Activity orchestrator (daemon in --dry-run, sends read from its log):
- a new request no longer drops the tail of the last transition (the commands after its Switched
  marker): they go out first, in order
- a switch between two activities that share a device leaves out that device's power commands
  from the copied stop and start sequences
- a step whose command cannot be sent is reported (activity_progress "error", "failedSteps" on
  the "Transition completed" frame)
- the running activity cannot be deleted; after a reload without it the Hub reports PowerOff
- an activity step of type BTCommand goes to its device's Bluetooth host
- POST /api/activity-progress sends an activity_progress frame built from known fields, never the
  body as it came
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 9125
STREAMER = "00:11:22:33:44:55"  # placeholder address
OTHER_POWERON = "F9470P0123S0456P0123S0789P0123S9999"  # device 5's code: must never go to device 1

failures = []


def check(cond, name, detail=""):
    print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
    if not cond:
        failures.append(name)


def long_raw(pairs, mark=0x230, space=0x231):
    return "F9470" + "".join("P%04XS%04X" % (mark, space) for _ in range(pairs))


def pronto(pairs):
    words = ["0000", "006D", "%04X" % pairs, "0000"] + ["0016", "0041"] * pairs
    return " ".join(words)


def config_dir():
    d = tempfile.mkdtemp(prefix="orch_dispatch_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devices["devices"][0]["commands"].append({"name": "LongCode", "raw": long_raw(450), "port": 7})
    devices["devices"][0]["commands"].append({"name": "TooLong", "raw": long_raw(1100), "port": 7})
    # a device NAMED "1", listed before the device whose id is "1"
    devices["devices"].insert(0, {"id": "5", "name": "1", "type": "Switch",
                                  "commands": [{"name": "PowerOn", "raw": OTHER_POWERON, "port": 7}]})
    devices["devices"].append({"id": "3", "name": "Streamer", "type": "BT", "bdaddr": STREAMER,
                               "commands": [{"name": "Home", "raw": "HID:0C:0223"}]})
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({
        "id": "12", "name": "SwitchedAct", "type": "Custom", "order": 3, "deviceIds": ["1"],
        "startSequence": [
            {"type": "Command", "deviceId": "1", "command": "PowerOn"},
            {"type": "Switched"},
            {"type": "Delay", "delay": 600},
            {"type": "Command", "deviceId": "1", "command": "InputHdmi1"},
        ],
        "stopSequence": [
            {"type": "Command", "deviceId": "1", "command": "PowerOff"},
            {"type": "Switched"},
            {"type": "Delay", "delay": 400},
            {"type": "Command", "deviceId": "1", "command": "VolumeDown"},
        ],
    })
    acts["activities"].append({
        "id": "13", "name": "StreamKey", "type": "Custom", "order": 5,
        "startSequence": [{"type": "BTCommand", "deviceId": "3", "command": "Home"}],
        "stopSequence": [],
    })
    acts["activities"].append({
        "id": "18", "name": "Ghost", "type": "Custom", "order": 4,
        "startSequence": [
            {"type": "Command", "deviceId": "99", "command": "PowerOn"},
            {"type": "Command", "deviceId": "1", "command": "NoSuchCommand"},
        ],
        "stopSequence": [],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    shutil.copy(os.path.join(SAMPLE_CONFIG, "settings.json"), os.path.join(d, "settings.json"))
    return d, acts


def wait_state(hub, activity, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        st, s = hub.http_get("/api/activity")
        if isinstance(s, dict) and str(s.get("activity")) == activity and s.get("state") in (0, 2) and not s.get("busy"):
            return True
        time.sleep(0.02)
    return False


def wait_idle_sent(hub, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        st, s = hub.http_get("/api/activity")
        if isinstance(s, dict) and not s.get("busy") and not s.get("sending"):
            return True
        time.sleep(0.05)
    return False


def log_since(path, mark):
    return open(path, errors="replace").read()[mark:]


def log_len(path):
    return len(open(path, errors="replace").read())


def ws_ask(ws, req, timeout=5.0):
    ws.send_json(req)
    end = time.time() + timeout
    while time.time() < end:
        m = ws.recv_json(timeout=max(0.1, end - time.time()))
        if m and m.get("type") == "ack" and m.get("msgId") == req.get("msgId"):
            return m
    return None


def pronto_bytes(text):
    vals = [ln for ln in text.splitlines() if "IR Pronto simulated:" in ln]
    if not vals:
        return None
    return int(vals[-1].split("IR Pronto simulated:")[1].split("bytes")[0].strip())


def main():
    cfg, acts = config_dir()
    log_path = os.path.join(cfg, "hub.log")
    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log_path) as hub:
            print("[-] the tail of the last transition is sent before the next one")
            st, _ = hub.http_post("/api/activity", {"activity": "12"})
            check(st == 200 and wait_state(hub, "12"), "12 reports running (start tail still due)")
            mark = log_len(log_path)
            st, _ = hub.http_post("/api/activity", {"activity": "10"})
            check(st == 200 and wait_state(hub, "10") and wait_idle_sent(hub), "switch to 10 right away")
            log = log_since(log_path, mark)
            tail_at = log.find("Tail of the last switch: Start tail: InputHdmi1")
            own_at = log.find("Starting: InputHdmi1")
            check(tail_at >= 0 and own_at > tail_at, "12's start tail (InputHdmi1) is sent, before 10's own steps",
                  "%d %d" % (tail_at, own_at))

            st, _ = hub.http_post("/api/activity", {"activity": "12"})
            check(wait_state(hub, "12") and wait_idle_sent(hub), "12 again")
            st, _ = hub.http_post("/api/poweroff", {})
            check(st == 200 and wait_state(hub, "-1"), "power off reports idle (stop tail still due)")
            mark = log_len(log_path)
            st, _ = hub.http_post("/api/activity", {"activity": "11"})
            check(st == 200 and wait_state(hub, "11") and wait_idle_sent(hub), "start 11 right away")
            log = log_since(log_path, mark)
            check("Tail of the last switch: Stop tail: VolumeDown" in log, "12's stop tail (VolumeDown) is sent")

            print("[-] shared devices keep their power")
            st, _ = hub.http_post("/api/poweroff", {})
            wait_state(hub, "-1")
            wait_idle_sent(hub)
            hub.http_post("/api/activity", {"activity": "10"})
            check(wait_state(hub, "10") and wait_idle_sent(hub), "10 running")
            st, r = hub.http_post("/api/activity", {"activity": "11", "dry_run": True})
            steps = r if isinstance(r, dict) else {}
            names = [(s.get("deviceId"), s.get("command")) for s in steps.get("steps", [])]
            check(("1", "PowerOff") not in names and ("1", "PowerOn") not in names,
                  "10 -> 11: no PowerOff / PowerOn for the TV both use", str(names))
            check(("2", "PowerToggle") in names, "the media player (only in 11) is still powered", str(names))
            st, r = hub.http_post("/api/activity", {"activity": "-1", "dry_run": True})
            steps = r if isinstance(r, dict) else {}
            names = [(s.get("deviceId"), s.get("command")) for s in steps.get("steps", [])]
            check(("1", "PowerOff") in names, "power off still sends the TV's PowerOff", str(names))

            print("[-] steps that cannot be sent are reported")
            ws = hub.ws()
            hub.http_post("/api/activity", {"activity": "18"})
            m, frames = ws.wait_for("activity_progress", 10,
                                    lambda m: m.get("desc") == "Transition completed")
            errs = [f for f in frames if f.get("type") == "activity_progress" and f.get("error")]
            check(m is not None and m.get("failedSteps") == 2, "Transition completed carries failedSteps 2", str(m))
            check(len(errs) == 2 and sorted(f.get("error") for f in errs) == [-6, -5],
                  "the two steps carry their error (-5 device, -6 command)", str(errs))
            wait_idle_sent(hub)

            print("[-] a BTCommand step goes to its device's Bluetooth host")
            mark = log_len(log_path)
            hub.http_post("/api/activity", {"activity": "13"})
            check(wait_state(hub, "13") and wait_idle_sent(hub), "13 running")
            log = log_since(log_path, mark)
            check("bdaddr=%s script=Home" % STREAMER in log, "the key names the host of device 3",
                  [ln for ln in log.splitlines() if "BT Send" in ln].__repr__())

            print("[-] the running activity cannot be deleted; a reload without it is PowerOff")
            hub.http_post("/api/activity", {"activity": "10"})
            check(wait_state(hub, "10") and wait_idle_sent(hub), "10 running")
            st, r = hub.http_post("/api/activity-delete", {"id": "10"})
            check(st == 409, "deleting the running activity: 409", "%s %s" % (st, r))
            st, s = hub.http_get("/api/activity")
            check(isinstance(s, dict) and s.get("activity") == "10", "still running 10", str(s))
            restored = {"activities": [a for a in acts["activities"] if a["id"] != "10"]}
            with open(os.path.join(hub.data_dir, "config", "activities.json"), "w") as f:
                json.dump(restored, f)
            hub.http_post("/api/sync", {})
            check(wait_state(hub, "-1", 5.0), "after a reload without 10 the Hub reports PowerOff")
            with open(os.path.join(hub.data_dir, "config", "activities.json"), "w") as f:
                json.dump(acts, f)
            hub.http_post("/api/sync", {})

            print("[-] POST /api/activity-progress")
            ws.drain()
            forged = {"type": "activity", "data": {"activity": "FORGED", "state": 2}, "forged": True,
                      "activity": "10", "state": "running", "desc": "from a client"}
            st, _ = hub.http_post("/api/activity-progress", forged)
            m, frames = ws.wait_for("activity_progress", 3, lambda m: m.get("desc") == "from a client")
            check(st == 200 and m is not None and m.get("activity") == "10" and m.get("state") == "running",
                  "an activity_progress frame with the checked fields", str(m))
            check(not any(f.get("type") == "activity" or f.get("forged") for f in frames),
                  "the body itself is not sent", str(frames))
            st, _ = hub.http_post("/api/activity-progress", None, raw=b"not json")
            check(st == 400, "a body that is not a JSON object: 400", str(st))

            ws.close()
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("activity orchestrator: %d FAILED" % len(failures))
        return 1
    print("all activity orchestrator checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
