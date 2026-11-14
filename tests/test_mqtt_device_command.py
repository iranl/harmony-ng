#!/usr/bin/env python3
"""
A command to an MQTT-type device (mqttEnabled) is answered at once and gives one button event on
the bus, whichever thread sends it. The dispatcher used to post /api/button-press to the daemon's
own port: from the main loop (WebSocket and MQTT commands, activity steps) nobody accepted that
connection, so the main loop waited 2 s per command (no request answered meanwhile) and the
command was reported as failed when no broker was connected.
The daemon runs with --dry-run and MQTT off.
"""

import json
import os
import shutil
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8974


def config_dir():
    d = tempfile.mkdtemp(prefix="mqtt_dev_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devices["devices"].append({"id": "3", "name": "Lamp", "type": "MQTT", "mqttEnabled": True,
                               "mqttTopic": "home/lamp", "commands": [{"name": "PowerToggle"}]})
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    for name in ("activities.json", "settings.json"):
        shutil.copy(os.path.join(SAMPLE_CONFIG, name), os.path.join(d, name))
    return d


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    cfg = config_dir()
    try:
        with HubHarness(port=PORT, config_dir=cfg) as hub:
            ev = hub.ws()
            ev.recv_json(5)  # snapshot
            cmd = hub.ws()
            cmd.recv_json(5)

            # WebSocket send_command runs on the main loop
            health = {}

            def probe():
                time.sleep(0.2)
                t = time.time()
                st, _ = hub.http_get("/health", timeout=5)
                health["status"], health["took"] = st, time.time() - t

            th = threading.Thread(target=probe)
            t0 = time.time()
            th.start()
            cmd.send_json({"action": "send_command", "deviceId": "3", "command": "PowerToggle", "msgId": 1})
            ack, _ = cmd.wait_for("ack", 5)
            took = time.time() - t0
            th.join()
            check(ack is not None and ack.get("ok") is True, "WebSocket send_command to the MQTT device: ok", str(ack))
            check(took < 1.0, "answered at once (no 2 s wait on the main loop)", "%.2f s" % took)
            check(health.get("status") == 200 and health.get("took", 9) < 1.0,
                  "GET /health during the command answered at once", str(health))
            btn, seen = ev.wait_for("button", 3)
            check(btn is not None and btn.get("button") == "PowerToggle" and btn.get("deviceId") == "3"
                  and btn.get("action") == "press" and btn.get("source") == "api",
                  "one button event for the command", str(btn))
            check(len([m for m in ev.drain(0.5) if m.get("type") == "button"]) == 0, "and only one")

            # REST /api/ir-send runs on a worker thread: same result
            t0 = time.time()
            st, r = hub.http_post("/api/ir-send", {"deviceId": "3", "command": "PowerToggle"}, timeout=5)
            check(st == 200 and r.get("ok") is True and time.time() - t0 < 1.0, "REST ir-send to the MQTT device: ok at once", "%s %s" % (st, r))
            btn, _ = ev.wait_for("button", 3)
            check(btn is not None and btn.get("button") == "PowerToggle", "button event for the REST command", str(btn))
            ev.close()
            cmd.close()
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("mqtt device command: %d FAILED" % len(failures))
        return 1
    print("all mqtt device command checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
