#!/usr/bin/env python3
"""
hub_rf pushes its RF status object to hub_daemon (POST /api/rf/status-changed) whenever it changed;
the daemon emits rf_status on the bus (WebSocket clients, the snapshot, MQTT <base>/rf retained).
The push used to go through a 1 KB request buffer in hub_rf's client: a status object of more than
about 900 bytes (any Hub with link counters) arrived cut, the daemon answered 400 and no rf_status
was ever emitted after the start-up baseline.

Checks, with hub_rf's real client (tests/rf_push_client.c, rf_state.c http_request_local):
- a 3 KB status object with a changed battery is answered 200 / changed true, reaches a WebSocket
  client as rf_status and MQTT <base>/rf as a retained message, and is in the snapshot;
- the same object again: changed false, nothing emitted;
- a body sent in several pieces (more than one TCP read on the daemon side) is read whole;
- a body that is not a status object is answered 400 and logged once per streak.
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, WsClient, REPO_ROOT, BUILD_DIR  # noqa: E402
from mock_mqtt import MockBroker, MockClient  # noqa: E402

PORT = 8983
BASE = "harmony/hub"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def build_client():
    out = os.path.join(BUILD_DIR, "rf_push_client")
    src = os.path.join(REPO_ROOT, "payload", "source")
    os.makedirs(BUILD_DIR, exist_ok=True)
    subprocess.run(["gcc", "-Wall", "-O2", "-I" + src, os.path.join(REPO_ROOT, "tests", "rf_push_client.c"),
                    os.path.join(src, "rf_state.c"), os.path.join(src, "cJSON.c"), "-lpthread", "-lm", "-o", out],
                   check=True)
    return out


def status(percent, charging, voltage, pad=0):
    """An RF status object as hub_rf builds it (dummy addresses); pad makes it larger"""
    slot = lambda n, active: {  # noqa: E731
        "slot": n, "name": "Harmony Remote (Slot %d)" % n, "model": "unknown", "has_screen": True,
        "supports_ota": True, "active": active, "in_sync": active, "address": "E5F60718" if active else "None",
        "rf_address": "E5F60718" if active else "None", "sw_version": "2.3.0" if active else "unknown",
        "config_version": 7 if active else 0,
        "battery": {"percent": percent if active else 0, "charging": charging if active else False,
                    "voltage_mv": voltage if active else 0},
        "link": {"frames_rx": 120, "acked_sent": 12, "acked_ok": 12, "retries": 0, "naks": 0, "last_seen_s": 2}}
    st = {"running": True, "ok": True, "fw_version": "v39.00", "paired": True, "rf_address": "A1B2C3D4",
          "hub_rf_address": "A1B2C3D4", "pairing_active": False, "sync_pending": False, "sync_pending_slot": 0,
          "sync_pending_diff": "Hub configuration changed. Push settings to sync." + " " * pad,
          "hub_config_version": 7,
          "ota": {"in_progress": False, "slot": 0, "status": 0, "sent_bytes": 0, "total_bytes": 0,
                  "sent_chunks": 0, "total_chunks": 0, "error": ""},
          "slots": [slot(1, True), slot(2, False)]}
    return json.dumps(st, separators=(",", ":"))


def push(client, body):
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(body)
        path = f.name
    try:
        res = subprocess.run([client, str(PORT), "/api/rf/status-changed", path], capture_output=True, text=True, timeout=10)
    finally:
        os.remove(path)
    code, _, reply = res.stdout.strip().partition(" ")
    try:
        return int(code), json.loads(reply) if reply else {}
    except ValueError:
        return int(code) if code.lstrip("-").isdigit() else -1, {}


def push_in_pieces(body, piece=200, gap=0.05):
    """The same request a client sends, written in small pieces with pauses: the daemon gets the
    body in more than one read"""
    data = body.encode()
    head = ("POST /api/rf/status-changed HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n" % len(data)).encode()
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(head)
    for i in range(0, len(data), piece):
        time.sleep(gap)
        s.sendall(data[i:i + piece])
    reply = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        reply += chunk
    s.close()
    head_r, _, body_r = reply.partition(b"\r\n\r\n")
    return int(head_r.split(b" ")[1]), json.loads(body_r or b"{}")


def next_rf(ws, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            m = ws.recv_json(timeout=max(0.1, deadline - time.time()))
        except Exception:
            continue
        if m and m.get("type") == "rf_status":
            return m
    return None


def main():
    client = build_client()
    broker = MockBroker()
    log = tempfile.NamedTemporaryFile(prefix="rfpush_", suffix=".log", delete=False).name
    mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": broker.port, "baseTopic": BASE, "clientId": "harmony-test",
                "haDiscovery": False}
    try:
        with HubHarness(port=PORT, env={"HARMONY_MQTT_LIVE": "1"}, extra_data_files={"mqtt.json": mqtt_cfg},
                        log_path=log) as hub:
            deadline = time.time() + 10
            while time.time() < deadline and (BASE + "/status") not in broker.retained:
                time.sleep(0.1)
            sub = MockClient("127.0.0.1", broker.port, "observer")
            sub.subscribe(BASE + "/rf")
            ws = WsClient("127.0.0.1", PORT)
            ws.recv_json(timeout=5)  # snapshot

            print("[-] a 3 KB status object through hub_rf's client")
            body = status(87, False, 3980, pad=1800)
            check(len(body) > 2048, "body well past the old 1 KB request buffer (%d bytes)" % len(body))
            code, reply = push(client, body)
            check(code == 200 and reply.get("changed") is True, "200, changed true", "%s %s" % (code, reply))
            m = next_rf(ws, 3)
            check(m is not None and m["slots"][0]["battery"]["percent"] == 87, "rf_status on the WebSocket", str(m)[:200])
            check(m is not None and "link" in m["slots"][0], "the event carries the link counters (API.md 10.12)")
            mq = sub.wait_for(BASE + "/rf", 5, lambda p: (json.loads(p) or {}).get("slots", [{}])[0].get("battery", {}).get("percent") == 87)
            check(mq is not None, "rf_status on MQTT <base>/rf", str(mq)[:120])
            r = broker.retained.get(BASE + "/rf")
            check(r is not None and json.loads(r)["slots"][0]["battery"]["percent"] == 87, "MQTT rf is retained")

            print("[-] the same object again: no event")
            code, reply = push(client, body)
            check(code == 200 and reply.get("changed") is False, "200, changed false", "%s %s" % (code, reply))
            check(next_rf(ws, 1.5) is None, "no rf_status emitted")

            print("[-] only link counters and voltage differ: no event")
            moved = json.loads(body)
            moved["slots"][0]["link"]["frames_rx"] = 999
            moved["slots"][0]["battery"]["voltage_mv"] = 3975
            code, reply = push(client, json.dumps(moved, separators=(",", ":")))
            check(code == 200 and reply.get("changed") is False, "200, changed false", "%s %s" % (code, reply))
            check(next_rf(ws, 1.5) is None, "no rf_status emitted")

            print("[-] charging, body in several pieces")
            code, reply = push_in_pieces(status(88, True, 4120, pad=1800))
            check(code == 200 and reply.get("changed") is True, "200, changed true", "%s %s" % (code, reply))
            m = next_rf(ws, 3)
            check(m is not None and m["slots"][0]["battery"]["charging"] is True, "charging on the WebSocket", str(m)[:200])
            ws.send_json({"action": "get_snapshot", "id": 7})
            snap = None
            deadline = time.time() + 5
            while time.time() < deadline and snap is None:
                f = ws.recv_json(timeout=2)
                if f and f.get("type") == "snapshot":
                    snap = f
            check(snap is not None and snap["rf_status"]["slots"][0]["battery"]["charging"] is True,
                  "the snapshot carries the pushed status", str(snap)[:200] if snap else "no snapshot")

            print("[-] a body that is not a status object")
            code, _ = push(client, "{\"slots\": [")
            check(code == 400, "400 for a cut body", str(code))
            push(client, "not json")
            time.sleep(0.3)
            with open(log, errors="replace") as f:
                text = f.read()
            check(text.count("body is not a status object") == 1, "rejection logged once per streak",
                  str(text.count("body is not a status object")))
            code, reply = push(client, status(89, True, 4130))
            check(code == 200 and reply.get("changed") is True, "accepted again")
            with open(log, errors="replace") as f:
                text = f.read()
            check("status objects accepted again" in text, "recovery logged")
            ws.sock.close()
            sub.close()
    finally:
        broker.close()
        try:
            os.remove(log)
        except OSError:
            pass
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all RF status push checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
