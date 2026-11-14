#!/usr/bin/env python3
"""
Test activity switch timing, Switched marker, tail execution, and activity mirror/target:
- Tail steps after Switched / Stop Switched marker execute
- While switching, /tmp/current_activity stays at the current activity until Switched marker
- /tmp/activity_target holds the target during transition and is unlinked upon completion
- a step's delay runs from the end of its send: equal commands in a row are all sent
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
MIRROR = "/tmp/current_activity"
TARGET = "/tmp/activity_target"


def config_dir():
    d = tempfile.mkdtemp(prefix="act_timing_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
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
            {"type": "Command", "deviceId": "1", "command": "InputHdmi2"},
        ],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


def repeat_config_dir():
    """Activity 14 sends VolumeUp four times, 150 ms apart; the TV is always on, so the builder
    adds no power steps and the bit stream holds only the four frames."""
    d = tempfile.mkdtemp(prefix="act_repeat_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    for dev in devices["devices"]:
        if str(dev.get("id")) == "1":
            dev["isPowerAlwaysOn"] = True
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({
        "id": "14", "name": "VolumeRamp", "type": "Custom", "order": 4, "deviceIds": ["1"],
        "startSequence": [{"type": "Command", "deviceId": "1", "command": "VolumeUp", "delay": 150}] * 4,
        "stopSequence": [],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


def tx_size(hub):
    try:
        return os.path.getsize(os.path.join(hub.mock_dir, "i2s_tx"))
    except OSError:
        return 0


def check_step_delay_after_send(check):
    """A step's delay runs from the end of its send. Each send takes 200 ms here (the mock's
    drain wait), longer than the 150 ms delay: counted from before the send, the next equal
    command came at once and the dispatcher's 90 ms same-button filter dropped it."""
    cfg = repeat_config_dir()
    try:
        with HubHarness(port=PORT, config_dir=cfg) as hub:
            st, r = hub.http_post("/api/settings", {"dry_run": False})
            check(st == 200 and r.get("dry_run") is False, "dry run off (real sends to the mock /dev/i2s)", "%s %s" % (st, r))
            with open(os.path.join(hub.mock_dir, "i2s_tx_ms"), "w") as f:
                f.write("200\n")
            st, r = hub.http_post("/api/ir-send", {"deviceId": "1", "command": "VolumeUp"}, timeout=5)
            one = tx_size(hub)
            check(st == 200 and one > 0, "one VolumeUp frame sent", "%s %s size %d" % (st, r, one))
            time.sleep(0.3)
            st, _ = hub.http_post("/api/activity", {"activity": "14"})
            check(st == 200, "start activity 14 (VolumeUp x4, 150 ms apart)")
            check(wait_state(hub, "14", timeout=10), "activity 14 reaches running")
            frames = (tx_size(hub) - one) // one if one else 0
            check(frames == 4, "all four VolumeUp steps were sent", "%d of 4 (bit stream %d bytes, one frame %d)" % (frames, tx_size(hub), one))
    finally:
        shutil.rmtree(cfg, ignore_errors=True)


def read_file(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def state_of(hub):
    st, body = hub.http_get("/api/activity")
    return body if isinstance(body, dict) else {}


def wait_state(hub, activity, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        s = state_of(hub)
        if str(s.get("activity")) == activity and s.get("state") in (0, 2) and not s.get("busy"):
            return True
        time.sleep(0.05)
    return False


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
            # 1. Start activity 12
            st, _ = hub.http_post("/api/activity", {"activity": "12"})
            check(st == 200, "start activity 12 requested")

            # Activity target written immediately
            time.sleep(0.05)
            check(read_file(TARGET) == "12", "target file holds 12 during switch", str(read_file(TARGET)))

            # Wait until activity 12 running
            check(wait_state(hub, "12"), "activity 12 reaches running")
            check(read_file(MIRROR) == "12", "current_activity updated to 12", str(read_file(MIRROR)))
            check(read_file(TARGET) is None, "target file cleared after transition", str(read_file(TARGET)))

            # Let delay step finish
            time.sleep(0.7)

            # 2. Power off
            st, _ = hub.http_post("/api/poweroff", {})
            check(st == 200, "power off requested")

            time.sleep(0.05)
            check(read_file(MIRROR) == "12", "while power off starts, current_activity is still 12", str(read_file(MIRROR)))
            check(read_file(TARGET) == "-1", "target file holds -1 during power off", str(read_file(TARGET)))

            check(wait_state(hub, "-1"), "activity reaches idle / powered off")
            check(read_file(MIRROR) == "-1", "current_activity updated to -1", str(read_file(MIRROR)))
            check(read_file(TARGET) is None, "target file cleared after power off", str(read_file(TARGET)))

            time.sleep(0.5)

        log = open(log_path, errors="replace").read()
        check("InputHdmi1" in log, "start tail step executed")
        check("InputHdmi2" in log, "stop tail step executed (STEP_ACT_STOP_SWITCHED)")

        print("[-] step delay counted from the end of the send")
        check_step_delay_after_send(check)

    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("activity switch timing: %d FAILED" % len(failures))
        return 1
    print("all activity switch timing checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
