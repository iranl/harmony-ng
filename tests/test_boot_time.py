#!/usr/bin/env python3
"""
boot_time in /api/status, the WebSocket snapshot's hub object and the retained MQTT <base>/hub
object. The test build treats the clock as unset (a Hub before SNTP) while the file named by
HARMONY_TEST_CLOCK_UNSET_FILE exists: boot_time is null, then the file goes away (SNTP synced)
and the retained hub object is republished once with the boot time.
"""

import json
import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402
from mock_mqtt import MockBroker  # noqa: E402

PORT = 8993
BASE = "harmony/hub"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def host_uptime():
    with open("/proc/uptime") as f:
        return float(f.read().split()[0])


def hub_payloads(broker, start):
    """hub objects published since the broker log had `start` hub messages"""
    out = []
    for topic, payload in broker.messages(BASE + "/hub")[start:]:
        try:
            out.append(json.loads(payload.decode("utf-8")))
        except ValueError:
            pass
    return out


def main():
    broker = MockBroker()
    marker = tempfile.NamedTemporaryFile(prefix="clock_unset_", delete=False)
    marker.close()
    mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": broker.port, "baseTopic": BASE,
                "clientId": "harmony-test", "discoveryPrefix": "homeassistant", "haDiscovery": False}
    try:
        print("[-] boot_time while the clock is not set, then after the sync")
        env = {"HARMONY_MQTT_LIVE": "1", "HARMONY_TEST_CLOCK_UNSET_FILE": marker.name}
        with HubHarness(port=PORT, env=env, extra_data_files={"mqtt.json": mqtt_cfg}) as hub:
            deadline = time.time() + 10
            while time.time() < deadline and (BASE + "/hub") not in broker.retained:
                time.sleep(0.1)
            first = json.loads(broker.retained[BASE + "/hub"].decode()) if (BASE + "/hub") in broker.retained else {}
            check("boot_time" in first and first["boot_time"] is None, "retained hub: boot_time null before the clock is set", str(first)[:200])
            check(isinstance(first.get("uptime_sec"), (int, float)), "retained hub still has uptime_sec")
            st, status = hub.http_get("/api/status")
            check(st == 200 and "boot_time" in status and status["boot_time"] is None, "/api/status: boot_time null", str(status)[:200])

            start = len(broker.messages(BASE + "/hub"))
            os.remove(marker.name)  # SNTP has set the clock
            got = None
            deadline = time.time() + 12
            while time.time() < deadline and got is None:
                for p in hub_payloads(broker, start):
                    if isinstance(p.get("boot_time"), (int, float)):
                        got = p
                time.sleep(0.2)
            expect = time.time() - host_uptime()
            check(got is not None, "hub republished once the clock is set")
            if got:
                check(abs(got["boot_time"] - expect) <= 3, "boot_time = now - uptime (%d vs %d)" % (got["boot_time"], expect))
                check(got.get("ts", 0) >= 1704067200, "the republished ts is a real time")
            ret = json.loads(broker.retained[BASE + "/hub"].decode())
            check(isinstance(ret.get("boot_time"), (int, float)), "the retained copy now has boot_time")
            st, status = hub.http_get("/api/status")
            check(st == 200 and isinstance(status.get("boot_time"), (int, float))
                  and abs(status["boot_time"] - expect) <= 3, "/api/status boot_time after the sync", str(status.get("boot_time")))
            ws = hub.ws()
            snap, frames = ws.wait_for("snapshot", 5)
            hb = (snap or {}).get("hub", {})
            check(isinstance(hb.get("boot_time"), (int, float)) and abs(hb["boot_time"] - expect) <= 3,
                  "WebSocket snapshot hub has boot_time", str(hb)[:200])
            ws.close()
            check(len(hub_payloads(broker, start)) == 1, "republished exactly once", str(len(hub_payloads(broker, start))))
            # stable: no republish while the clock stays put
            n = len(broker.messages(BASE + "/hub"))
            time.sleep(7)
            again = hub_payloads(broker, n)
            check(again == [], "no further republish while the clock stays put", str(again)[:200])
    finally:
        broker.close()
        try:
            os.remove(marker.name)
        except OSError:
            pass
    print("%s" % ("all boot_time checks passed" if not failed else "%d boot_time checks FAILED" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
