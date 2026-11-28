#!/usr/bin/env python3
"""
A sequence step that holds its key ("hold", ms) instead of tapping it (orchestrator):
- a device's power-off step with a hold presses the key, keeps it down and releases it when the
  hold is up; the transition ends after the release
- a power off with such a step takes at least the hold
- an activity saved with a held step keeps the hold, capped at 10 s; a raw Bluetooth (HID:) step
  keeps no hold
- Test Live (POST /api/activity-test-sequence) holds a key for its hold, given as a number or as
  a string holding one
The daemon runs with --dry-run: the infrared sends are simulated (a Bluetooth send fails there,
as hub_btstack does not run, so the held step uses an infrared device).
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8984
HOLD_MS = 1500


def config_dir():
    d = tempfile.mkdtemp(prefix="step_hold_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    player = next(dv for dv in devices["devices"] if str(dv["id"]) == "2")  # infrared Media Player
    player["powerOffSequence"] = [{"type": "Command", "deviceId": "2", "command": "PowerToggle", "hold": HOLD_MS}]
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({
        "id": "12", "name": "Play", "type": "Custom", "order": 3, "deviceIds": ["2"],
        "startSequence": [{"type": "Command", "deviceId": "2", "command": "Home"}],
        "stopSequence": [],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


def state_of(hub):
    st, body = hub.http_get("/api/activity")
    return body if isinstance(body, dict) else {}


def wait_state(hub, activity, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        s = state_of(hub)
        if str(s.get("activity")) == activity and s.get("state") in (0, 2) and not s.get("busy") \
                and not s.get("sending"):
            return True
        time.sleep(0.05)
    return False


def saved_activity(hub, act_id):
    st, body = hub.http_get("/api/activities")
    acts = body.get("activities", []) if isinstance(body, dict) else []
    return next((a for a in acts if str(a.get("id")) == act_id), None)


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
            st, _ = hub.http_post("/api/activity", {"activity": "12"})
            check(st == 200, "start activity 12")
            check(wait_state(hub, "12"), "activity 12 running")

            t0 = time.time()
            st, _ = hub.http_post("/api/poweroff", {})
            check(st == 200, "power off")
            check(wait_state(hub, "-1"), "powered off")
            took = time.time() - t0
            check(took >= HOLD_MS / 1000.0, "the power off takes at least the hold", "%.2f s" % took)

            # An activity saved with held steps keeps the hold, capped; a raw HID step keeps none
            st, _ = hub.http_post("/api/activity-save", {
                "id": "12", "name": "Play",
                "startSequence": [{"type": "Command", "deviceId": "2", "command": "Home"}],
                "stopSequence": [
                    {"type": "Command", "deviceId": "2", "command": "PowerToggle", "hold": 2000},
                    {"type": "Command", "deviceId": "2", "command": "Home", "hold": 60000},
                    {"type": "BTCommand", "deviceId": "2", "command": "HID:0C:0030", "hold": 2000},
                ],
            })
            check(st == 200, "save activity 12 with held steps")

            # Test Live answers after the hold, given as a number or as a string holding one
            for hold in (600, "600"):
                t0 = time.time()
                st, r = hub.http_post("/api/activity-test-sequence", {"steps": [
                    {"type": "Command", "deviceId": "2", "command": "Home", "hold": hold}]}, timeout=10)
                took = time.time() - t0
                check(st == 200 and isinstance(r, dict) and r.get("executed") == 1 and took >= 0.6,
                      "Test Live holds the key %r ms" % (hold,), "%s %s %.2f s" % (st, r, took))
            act = saved_activity(hub, "12") or {}
            stop = act.get("stopSequence", [])
            holds = [s.get("hold") for s in stop]
            check(holds == [2000, 10000, None], "holds saved, capped at 10 s, none on a raw HID step", str(stop))

        log = open(log_path, errors="replace").read().splitlines()

        def first(pred):
            return next((i for i, l in enumerate(log) if pred(l)), -1)

        hold_i = first(lambda l: "Orch: hold 'PowerToggle'" in l and "for %d ms" % HOLD_MS in l)
        release_i = first(lambda l: "Orch: release held 'PowerToggle'" in l)
        not_sent = [l for l in log if "not sent" in l and "PowerToggle" in l]
        check(hold_i >= 0, "the power-off step holds PowerToggle")
        check(not not_sent, "the held key was sent", str(not_sent))
        check(release_i > hold_i >= 0, "the held key is released", "hold %d, release %d" % (hold_i, release_i))
        done_i = next((i for i, l in enumerate(log) if i > hold_i >= 0 and "Orch step" in l), -1)
        check(done_i < 0 or done_i > release_i, "no step of the power off is sent before the release",
              "release %d, next step %d" % (release_i, done_i))
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("step hold: %d FAILED" % len(failures))
        return 1
    print("all step hold checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
