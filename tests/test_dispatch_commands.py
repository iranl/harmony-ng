#!/usr/bin/env python3
"""
Command dispatch and IR codes (daemon in --dry-run, sends read from its log):
- device matching tries every id before any name
- Connect / Disconnect are accepted for a job only when the device lists them, as the sender
- dry run: the delayed Bluetooth reconnect script is not started
- long codes are sent whole or refused, never cut; a raw Harmony code given to ir-send is sent
- command jobs: total length capped, a queued command says so, cancel_commands / cancel_activity
  drop the jobs
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 9129
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
                               "btTriggerCommandName": "1:InputHdmi1",
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
            ws = hub.ws()
            print("[-] device matching and Bluetooth link commands")
            mark = log_len(log_path)
            st, r = hub.http_post("/api/ir-send", {"deviceId": "1", "command": "PowerOn"})
            log = log_since(log_path, mark)
            check(st == 200 and "IR Keycode simulated: 0000 006d" in log and OTHER_POWERON not in log,
                  "deviceId 1 is the device with id 1, not the one named \"1\"", "%s %s" % (st, log[-300:]))
            st, r = hub.http_post("/api/ir-send", {"deviceId": "Living Room TV", "command": "VolumeUp"})
            check(st == 200, "a name still finds its device", "%s %s" % (st, r))
            r = ws_ask(ws, {"action": "send_command", "msgId": 11, "deviceId": "2", "command": ["Home", "Connect"]})
            check(r is not None and r.get("ok") is False and r.get("code") == 404,
                  "Connect on an infrared device without it: refused, not acked and skipped", str(r))
            mark = log_len(log_path)
            r = ws_ask(ws, {"action": "send_command", "msgId": 12, "deviceId": "3", "command": "Connect"})
            time.sleep(0.2)
            log = log_since(log_path, mark)
            check(r is not None and r.get("ok") is True and ("connect_host " + STREAMER) in log,
                  "Connect on a Bluetooth device (the sanitizer lists it): sent", "%s %s" % (r, log[-300:]))

            print("[-] dry run: the delayed Bluetooth reconnect script is not started")
            hub.http_post("/api/activity", {"activity": "10"})
            check(wait_state(hub, "10") and wait_idle_sent(hub), "10 running")
            mark = log_len(log_path)
            st, r = hub.http_post("/api/ir-send", {"deviceId": "1", "command": "InputHdmi1"})
            time.sleep(0.2)
            log = log_since(log_path, mark)
            check(st == 200 and ("[DRY-RUN] BT reconnect simulated: %s in 2s" % STREAMER) in log,
                  "the trigger command prints the reconnect instead of starting the script", log[-300:])

            print("[-] long codes are sent whole or refused")
            mark = log_len(log_path)
            st, r = hub.http_post("/api/ir-send", {"deviceId": "1", "command": "LongCode"})
            log = log_since(log_path, mark)
            full = long_raw(450)
            check(st == 200 and ("IR Keycode simulated: " + full + " (") in log,
                  "a saved code of %d characters is sent whole" % len(full), "%s" % st)
            st, r = hub.http_post("/api/ir-send", {"deviceId": "1", "command": "TooLong"})
            check(st == 500, "a raw code with more timings than the encoder takes is refused", "%s %s" % (st, r))
            mark = log_len(log_path)
            st, r = hub.http_post("/api/ir-send", {"raw": OTHER_POWERON})
            log = log_since(log_path, mark)
            check(st == 200 and ("IR Keycode simulated: " + OTHER_POWERON + " (") in log,
                  "a raw Harmony code (F<carrier>P..S..) given to ir-send is sent", "%s %s" % (st, r))
            st, r = hub.http_post("/api/ir-test-learned", {"deviceId": "1", "raw": long_raw(1100)})
            check(st == 400, "ir-test-learned refuses it too, instead of playing a cut copy", "%s %s" % (st, r))
            mark = log_len(log_path)
            st, _ = hub.http_post("/api/pronto-blast", {"pronto": pronto(50)})
            half = pronto_bytes(log_since(log_path, mark))
            mark = log_len(log_path)
            st, _ = hub.http_post("/api/pronto-blast", {"pronto": pronto(100)})
            whole = pronto_bytes(log_since(log_path, mark))
            check(st == 200 and half and whole and whole > half,
                  "a Pronto code of %d characters is sent whole" % len(pronto(100)), "%s %s %s" % (st, half, whole))

            print("[-] command jobs")
            r = ws_ask(ws, {"action": "send_command", "msgId": 21, "deviceId": "2", "command": "Home",
                            "num_repeats": 50, "delay_secs": 60})
            check(r is not None and r.get("code") == 400, "a job of 50 minutes is refused", str(r))
            r = ws_ask(ws, {"action": "send_command", "msgId": 22, "deviceId": "2", "command": ["Home", "Select"],
                            "num_repeats": 3, "delay_secs": 2})
            check(r is not None and r.get("ok") is True and r.get("state") == "started", "a job starts", str(r))
            r = ws_ask(ws, {"action": "send_command", "msgId": 23, "deviceId": "2", "command": "Home"})
            check(r is not None and r.get("ok") is True and r.get("state") == "queued",
                  "a single command behind it says queued", str(r))
            r = ws_ask(ws, {"action": "cancel_commands", "msgId": 24})
            check(r is not None and r.get("ok") is True and r.get("commandsCancelled") == 2, "cancel_commands drops both", str(r))
            ws_ask(ws, {"action": "send_command", "msgId": 25, "deviceId": "2", "command": ["Home", "Select"],
                        "num_repeats": 3, "delay_secs": 2})
            r = ws_ask(ws, {"action": "cancel_activity", "msgId": 26})
            check(r is not None and r.get("ok") is True and r.get("commandsCancelled") == 1,
                  "cancel_activity drops the jobs too", str(r))
            ws.close()
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("command dispatch: %d FAILED" % len(failures))
        return 1
    print("all command dispatch checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
