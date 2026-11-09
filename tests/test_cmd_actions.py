#!/usr/bin/env python3
"""
Command-channel actions with REST equivalents (spec 5.3): update_check, update_apply,
activity_help, activity_help_answer, over MQTT <base>/cmd and <base>/cmd/response and over the
WebSocket. Same fields, codes and errors as GET /api/update-status, POST /api/update-apply,
POST /api/activity/help and POST /api/activity/help/answer.

update_check runs on a thread (it blocks for seconds on the Hub): with HARMONY_TEST_UPDATE_CHECK_MS
the test build simulates a slow check; a ping sent meanwhile is answered first and a second check
gets 409. The test build never runs the updater or the network check.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, WsClient  # noqa: E402
from mock_mqtt import MockBroker, MockClient  # noqa: E402

PORT = 8982
BASE = "harmony/hub"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def jload(p):
    try:
        return json.loads(p)
    except (TypeError, ValueError):
        return None


def ask(cmd, req, timeout=5.0):
    cmd.publish(BASE + "/cmd", json.dumps(req), qos=1)
    m = cmd.wait_for(BASE + "/cmd/response", timeout, lambda p: (jload(p) or {}).get("id") == req["id"])
    return jload(m[1]) if m else None


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
    broker = MockBroker()
    mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": broker.port, "baseTopic": BASE, "clientId": "harmony-test",
                "haDiscovery": False}
    try:
        with HubHarness(port=PORT, env={"HARMONY_MQTT_LIVE": "1", "HARMONY_TEST_UPDATE_CHECK_MS": "1500"},
                        extra_data_files={"mqtt.json": mqtt_cfg}) as hub:
            deadline = time.time() + 10
            while time.time() < deadline and (BASE + "/status") not in broker.retained:
                time.sleep(0.1)
            cmd = MockClient("127.0.0.1", broker.port, "commander")
            cmd.subscribe(BASE + "/cmd/response")
            cmd.subscribe(BASE + "/activity_state")

            print("[-] update_check over MQTT (a newer version known)")
            r = ask(cmd, {"id": "u1", "action": "update_check", "remote_version": "99.0.0"})
            check(r is not None and r.get("ok") is True and r.get("action") == "update_check", "answered ok", str(r)[:200])
            st, rest = hub.http_get("/api/update-status")
            for k in ("current_version", "remote_version", "update_available", "updating", "commit", "firmware"):
                check(r is not None and k in r and r.get(k) == rest.get(k), "field %s as GET /api/update-status" % k,
                      "%s vs %s" % ((r or {}).get(k), rest.get(k)))
            check(r is not None and r.get("update_available") is True and r.get("remote_version") == "99.0.0",
                  "the check's result is in the reply", str(r)[:200])
            check(r is not None and "Update available" in r.get("message", ""), "the check's message", str(r)[:200])

            print("[-] a slow update_check does not block the main loop")
            ws = WsClient("127.0.0.1", PORT)
            ws.recv_json(timeout=5)  # snapshot
            t0 = time.time()
            ws.send_json({"action": "update_check", "msgId": 41})
            ws.send_json({"action": "ping", "msgId": 42})
            pong = ws_until(ws, lambda m: m.get("msgId") == 42, 3)
            check(pong is not None and pong.get("type") == "pong" and time.time() - t0 < 1.0,
                  "ping answered during the check (%.2f s)" % (time.time() - t0), str(pong))
            busy = ask(cmd, {"id": "u2", "action": "update_check"})
            check(busy is not None and busy.get("ok") is False and busy.get("code") == 409, "a second check meanwhile: 409", str(busy))
            done = ws_until(ws, lambda m: m.get("msgId") == 41, 5)
            check(done is not None and done.get("type") == "ack" and done.get("action") == "update_check" and done.get("ok") is True
                  and "current_version" in done and time.time() - t0 >= 1.4,
                  "the WebSocket client gets the check's reply when it ends (%.2f s)" % (time.time() - t0), str(done)[:200])
            ws.sock.close()
            r = ask(cmd, {"id": "u3", "action": "update_check"}, timeout=6)
            check(r is not None and r.get("ok") is True, "free again after the check", str(r)[:120])

            print("[-] update_apply")
            r = ask(cmd, {"id": "a1", "action": "update_apply"})
            check(r is not None and r.get("ok") is True and r.get("status") == "applying" and "message" in r,
                  "answered with the REST fields (the test build does not run the updater)", str(r))

            print("[-] activity_help and its answer")
            r = ask(cmd, {"id": "h0", "action": "activity_help", "activity": "11"})
            check(r is not None and r.get("ok") is False and r.get("code") == 409 and r.get("error") == "Activity is not running",
                  "not running: 409 as REST", str(r))
            r = ask(cmd, {"id": "h1", "action": "activity_help", "activity": "11", "dry_run": True})
            check(r is not None and r.get("ok") is True and r.get("activity") == "11"
                  and [d.get("id") for d in r.get("toggleDevices", [])] == ["2"] and isinstance(r.get("preview"), (dict, list)),
                  "dry run: the toggle device and the preview", str(r)[:300])
            r = ask(cmd, {"id": "s1", "action": "start_activity", "activity": "11"})
            check(r is not None and r.get("ok") is True, "start activity 11")
            cmd.wait_for(BASE + "/activity_state", 15, lambda p: (jload(p) or {}).get("state") == 2)
            r = ask(cmd, {"id": "h2", "action": "activity_help"})
            check(r is not None and r.get("ok") is True and r.get("activity") == "11"
                  and r.get("toggleDevices", [{}])[0].get("command") == "PowerToggle"
                  and r.get("toggleDevices", [{}])[0].get("name") == "Media Player",
                  "help on the running activity lists the toggle device", str(r)[:300])
            r = ask(cmd, {"id": "q1", "action": "activity_help_answer", "device": "1", "on": False})
            check(r is not None and r.get("ok") is False and r.get("code") == 404, "a device not asked about: 404", str(r))
            r = ask(cmd, {"id": "q2", "action": "activity_help_answer", "device": "2", "on": False})
            check(r is not None and r.get("ok") is True and r.get("sent") is True, "answer No: the toggle is sent", str(r))
            r = ask(cmd, {"id": "q3", "action": "activity_help_answer", "device": "2", "on": False})
            check(r is not None and r.get("code") == 404, "one answer per device", str(r))

            print("[-] the REST endpoints keep their shape")
            st, rr = hub.http_post("/api/activity/help", {"dry_run": True})
            check(st == 200 and rr.get("success") is True and "toggleDevices" in rr, "POST /api/activity/help", str(rr)[:200])
            st, rr = hub.http_post("/api/activity/help/answer", {"device": "9", "on": False})
            check(st == 404, "POST /api/activity/help/answer: 404", str(st))
            st, rr = hub.http_post("/api/update-check", {"remote_version": "1.0.0"})
            check(st == 200 and rr.get("has_update") is False and "message" in rr, "POST /api/update-check", str(rr)[:200])
            cmd.close()
    finally:
        broker.close()
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all command action checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
