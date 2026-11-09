#!/usr/bin/env python3
"""
WebSocket event tests against hub_daemon_test (spec docs/HA_INTEGRATION_FIRMWARE_SPEC.md,
milestone 1): activity_state and activity_progress on an activity switch (3.2), config_changed
and bt_status from the state watcher (3.3), button events from /api/button-press (3.4), rf_status from hub_rf's push to /api/rf/status-changed (3.5), the snapshot on connect and on
get_snapshot (3.9), ir_received while the IR capture mode is on (3.14).

Usage: python3 tests/test_ws_events.py            (builds hub_daemon_test if missing)
Pure stdlib; the daemon runs on a temporary mock root with tests/fixtures/ha/sample_config.
"""

import json
import subprocess
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402
import bt_status_seed  # noqa: E402
from ir_rx_signal import ir_rx_bitstream, nec_timings, sirc12_timings  # noqa: E402

PORT = 8997
g_passed = 0
g_failed = 0


def check(cond, name, detail=""):
    global g_passed, g_failed
    if cond:
        g_passed += 1
        print("  PASS %s" % name)
    else:
        g_failed += 1
        print("  FAIL %s %s" % (name, detail))


def states_of(frames):
    return [(m.get("state"), m.get("activity")) for m in frames if m.get("type") == "activity_state"]


def test_activity_switch(hub):
    print("[-] 3.2 activity_state and activity_progress on a dry-run activity switch")
    ws = hub.ws()
    time.sleep(0.2)
    ws.drain(0.3)
    # The daemon runs with --dry-run (IR simulated); "dry_run" in the body would only preview
    st, resp = hub.http_post("/api/activity", {"activity": "10"})
    check(st == 200 and resp.get("success") is True, "POST /api/activity 10", "%s %s" % (st, resp))
    running, frames = ws.wait_for("activity_state", 10, lambda m: m.get("state") == 2 and m.get("activity") == "10")
    check(running is not None, "activity_state running for 10 arrives", str(frames))
    sts = states_of(frames)
    check(sts and sts[0] == (1, "-1"), "first activity_state is starting with the old activity -1", str(sts))
    check(sts and sts[-1] == (2, "10"), "last activity_state is running with activity 10", str(sts))
    progress = [m for m in frames if m.get("type") == "activity_progress"]
    check(len(progress) >= 2, "activity_progress frames between them (%d)" % len(progress), str(frames))
    if progress:
        p = progress[0]
        check(p.get("activity") == "10" and p.get("activityId") == "10" and "stepDesc" in p and "totalSteps" in p,
              "activity_progress keeps its fields", str(p))
    if running:
        check(running.get("stateName") == "running" and running.get("busy") is False,
              "running frame: stateName and busy", str(running))
        check(isinstance(running.get("seq"), int) and isinstance(running.get("ts"), int),
              "seq and ts are integers", str(running))
    seqs = [m["seq"] for m in frames if "seq" in m]
    check(seqs == sorted(seqs) and len(set(seqs)) == len(seqs), "seq strictly increasing", str(seqs))
    st, cur = hub.http_get("/api/activity")
    check(st == 200 and cur.get("activity") == "10" and cur.get("state") == 2, "GET /api/activity agrees", str(cur))

    # Power off: stopping, then idle with activity -1
    st, resp = hub.http_post("/api/poweroff", {})
    check(st == 200, "POST /api/poweroff", "%s %s" % (st, resp))
    idle, frames = ws.wait_for("activity_state", 10, lambda m: m.get("state") == 0)
    sts = states_of(frames)
    check(sts and sts[0] == (3, "10"), "stopping with activity 10 first", str(sts))
    check(idle is not None and idle.get("activity") == "-1" and idle.get("stateName") == "idle",
          "idle with activity -1 last", str(idle))

    # A second client gets no replay: the bus pushes changes only
    ws2 = hub.ws()
    replay = ws2.drain(0.5)
    check(all(m.get("type") != "activity_state" for m in replay),
          "no activity_state replay for a new client without a switch", str(replay))
    ws2.close()
    ws.close()


BT_STATUS_FILE = "/tmp/hub_btstack_status.json"


def test_state_watcher(hub):
    print("[-] 3.3 config_changed and bt_status from the state watcher")
    ws = hub.ws()
    time.sleep(1.5)  # let the watcher take its baselines
    ws.drain(0.3)
    devices = os.path.join(hub.data_dir, "config", "devices.json")
    check(os.path.exists(devices), "sample devices.json under the mock root", devices)
    # Same content, newer mtime: the watcher compares mtime and size
    os.utime(devices, (time.time() + 5, time.time() + 5))
    msg, frames = ws.wait_for("config_changed", 5)
    check(msg is not None and msg.get("files") == ["devices"], "config_changed [devices]", str(frames))
    check(msg is not None and isinstance(msg.get("ts"), int), "config_changed carries ts", str(msg))
    extra = [m for m in ws.drain(1.5) if m.get("type") == "config_changed"]
    check(extra == [], "exactly one config_changed", str(extra))

    # Bluetooth: hub_btstack's status file appears, written by the real writer
    # (tests/bt_status_sample.c + bt_status_json.c); hub_daemon adds ok, running, connected, pairing
    try:
        bt_status_seed.write_status(BT_STATUS_FILE, last_button="KEY_ENTER", last_seen=1)
        msg, frames = ws.wait_for("bt_status", 5)
        check(msg is not None and msg.get("ok") is True and msg.get("running") is True
              and msg.get("connected") is True and msg.get("pairing") is False,
              "bt_status adds ok, running, connected and pairing", str(frames))
        check(msg is not None and [h.get("addr") for h in msg.get("hosts", [])] == ["AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:FF"]
              and msg.get("host", {}).get("name") == "Media Player" and msg.get("target") == "11:22:33:44:55:66",
              "bt_status keeps hub_btstack's fields (hosts with addr, host, target = the LE remote)", str(msg))
        st, rest = hub.http_get("/api/bt-status")
        check(st == 200 and rest.get("running") is True and rest.get("connected") is True
              and rest.get("pairing") is False and len(rest.get("hosts", [])) == 2,
              "GET /api/bt-status adds the same fields", str(rest))
        # Only volatile fields change: no new event
        bt_status_seed.write_status(BT_STATUS_FILE, last_button="KEY_UP", last_seen=2)
        extra = [m for m in ws.drain(1.5) if m.get("type") == "bt_status"]
        check(extra == [], "volatile fields do not count as a change", str(extra))
        # A real change does: the connected host drops
        bt_status_seed.write_status(BT_STATUS_FILE, host_connected=False, last_button="KEY_UP", last_seen=2)
        msg, frames = ws.wait_for("bt_status", 5)
        check(msg is not None and msg.get("connected") is False and msg.get("running") is True,
              "bt_status on a real change, connected false", str(frames))
        # Pairing mode shows at the top level
        bt_status_seed.write_status(BT_STATUS_FILE, host_connected=False, pairing=True, last_button="KEY_UP", last_seen=2)
        msg, frames = ws.wait_for("bt_status", 5)
        check(msg is not None and msg.get("pairing") is True, "pairing from host.pairing", str(frames))
        # A host name with quotes and a backslash: the file stays valid JSON
        bt_status_seed.write_status(BT_STATUS_FILE, host_name='Alex\'s "TV" \\ 2')
        msg, frames = ws.wait_for("bt_status", 5)
        check(msg is not None and msg.get("host", {}).get("name") == 'Alex\'s "TV" \\ 2',
              "host names are escaped by the writer", str(frames))
        # hub_btstack gone (its pid no longer exists): running false
        dead = subprocess.Popen(["true"])
        dead.wait()
        bt_status_seed.write_status(BT_STATUS_FILE, pid=dead.pid)
        msg, frames = ws.wait_for("bt_status", 5)
        check(msg is not None and msg.get("running") is False, "running false when hub_btstack's pid is gone", str(frames))
    finally:
        bt_status_seed.remove_status(BT_STATUS_FILE)
    ws.close()


def test_button_events(hub):
    print("[-] 3.4 button events from POST /api/button-press")
    ws = hub.ws()
    time.sleep(0.2)
    ws.drain(0.3)
    # What hub_rf posts for a mapped key: press, two hold repeats, release
    posts = [
        {"button": "VolumeUp", "action": "press", "source": "remote_rf", "slot": 1, "deviceId": "1", "command": "VolumeUp"},
        {"button": "VolumeUp", "action": "hold", "source": "remote_rf", "slot": 1, "deviceId": "1", "command": "VolumeUp"},
        {"button": "VolumeUp", "action": "hold", "source": "remote_rf", "slot": 1, "deviceId": "1", "command": "VolumeUp"},
        {"button": "VolumeUp", "action": "release", "source": "remote_rf", "slot": 1, "deviceId": "1", "command": "VolumeUp"},
    ]
    codes = []
    for p in posts:
        st, _ = hub.http_post("/api/button-press", p)
        codes.append(st)
    # Without a broker the MQTT mirror answers 503; the event does not depend on it
    check(all(c in (200, 503) for c in codes), "posts accepted (%s)" % codes)
    time.sleep(0.5)
    events = [m for m in ws.drain(0.5) if m.get("type") == "button"]
    check(len(events) == 2, "press and release give two button events, holds none (%d)" % len(events), str(events))
    if len(events) == 2:
        press, release = events
        check(press.get("action") == "press" and release.get("action") == "release", "actions in order", str(events))
        check(press.get("button") == "VolumeUp" and press.get("slot") == 1 and press.get("source") == "remote_rf",
              "button, slot and source", str(press))
        check(press.get("deviceId") == "1" and press.get("command") == "VolumeUp", "mapped target", str(press))
        check(press["seq"] < release["seq"], "seq order")
    # An unmapped key: no deviceId/command
    st, _ = hub.http_post("/api/button-press", {"button": "Menu", "action": "press", "source": "remote_rf", "slot": 2})
    msg, _ = ws.wait_for("button", 3)
    check(msg is not None and msg.get("button") == "Menu" and msg.get("slot") == 2 and "deviceId" not in msg and "command" not in msg,
          "unmapped key has no target", str(msg))
    # Missing key name: 400 and no event
    st, _ = hub.http_post("/api/button-press", {"action": "press", "source": "remote_rf"})
    check(st == 400, "400 without a key name (%s)" % st)
    check([m for m in ws.drain(0.5) if m.get("type") == "button"] == [], "no event on 400")
    ws.close()


def rf_status_sample(percent=80, frames_rx=10, voltage=3950):
    return {"running": True, "ok": True, "fw_version": "v39.00", "paired": True, "rf_address": "A1B2C3D4",
            "hub_rf_address": "A1B2C3D4", "pairing_active": False, "sync_pending": False, "sync_pending_slot": 0,
            "sync_pending_diff": "", "hub_config_version": 7,
            "ota": {"in_progress": False, "slot": 0, "status": 0, "sent_bytes": 0, "total_bytes": 0,
                    "sent_chunks": 0, "total_chunks": 0, "error": ""},
            "slots": [
                {"slot": 1, "name": "Harmony Ultimate (Slot 1)", "model": "ultimate", "model_name": "Harmony Ultimate",
                 "skin": 100, "equad_id": 16417, "has_screen": True, "supports_ota": True,
                 "active": True, "in_sync": True, "address": "E5F60718", "rf_address": "E5F60718", "sw_version": "2.3.0",
                 "config_version": 7, "battery": {"percent": percent, "charging": False, "voltage_mv": voltage},
                 "link": {"frames_rx": frames_rx, "acked_sent": 1, "acked_ok": 1, "retries": 0, "naks": 0, "last_seen_s": 2}},
                {"slot": 2, "name": "Harmony Remote (Slot 2)", "model": "unknown", "model_name": None,
                 "skin": None, "equad_id": None, "has_screen": True, "supports_ota": True,
                 "active": False, "in_sync": False, "address": "None", "rf_address": "None", "sw_version": "unknown",
                 "config_version": 0, "battery": {"percent": 0, "charging": False, "voltage_mv": 0},
                 "link": {"frames_rx": 0, "acked_sent": 0, "acked_ok": 0, "retries": 0, "naks": 0, "last_seen_s": -1}}]}


def test_rf_status_push(hub):
    print("[-] 3.5 rf_status from hub_rf's push to POST /api/rf/status-changed")
    ws = hub.ws()
    time.sleep(0.2)
    ws.drain(0.3)
    st, resp = hub.http_post("/api/rf/status-changed", rf_status_sample())
    check(st == 200 and resp.get("ok") is True and resp.get("changed") is True, "first push: changed", "%s %s" % (st, resp))
    msg, frames = ws.wait_for("rf_status", 3)
    check(msg is not None and msg.get("slots", [{}])[0].get("battery", {}).get("percent") == 80, "rf_status event", str(frames))
    check(msg is not None and msg["slots"][0]["battery"]["voltage_mv"] == 3950 and msg["slots"][0]["link"]["frames_rx"] == 10,
          "payload keeps voltage and link counters", str(msg))
    # Only link counters and voltage differ: no event
    st, resp = hub.http_post("/api/rf/status-changed", rf_status_sample(frames_rx=999, voltage=3800))
    check(st == 200 and resp.get("changed") is False, "same status: changed false", str(resp))
    check([m for m in ws.drain(1.0) if m.get("type") == "rf_status"] == [], "no rf_status for link/voltage only")
    # Battery percent changed: event
    st, resp = hub.http_post("/api/rf/status-changed", rf_status_sample(percent=79, frames_rx=1000))
    check(st == 200 and resp.get("changed") is True, "battery change: changed true", str(resp))
    msg, frames = ws.wait_for("rf_status", 3)
    check(msg is not None and msg["slots"][0]["battery"]["percent"] == 79, "second rf_status event", str(frames))
    # Bad body
    st, resp = hub.http_post("/api/rf/status-changed", None, raw=b"[1,2]")
    check(st == 400, "400 for a non-object body (%s)" % st)
    ws.close()


def test_snapshot(hub):
    print("[-] 3.9 snapshot as the first frame and on get_snapshot")
    ws = hub.ws()
    first = ws.recv_json(5)
    check(first is not None and first.get("type") == "snapshot", "first frame is the snapshot", str(first))
    if first:
        hubo = first.get("hub", {})
        for k in ("hub_id", "hub_name", "version", "api_version", "capabilities", "auth_enabled", "uptime_sec",
                  "legacy_enabled", "mdns_enabled"):
            check(k in hubo, "hub.%s present" % k, str(hubo))
        check("commit" not in hubo, "hub has no commit (status only)", str(hubo))
        check(hubo.get("api_version") == 2 and "snapshot" in hubo.get("capabilities", []), "api_version 2, snapshot flag", str(hubo))
        act = first.get("activity_state", {})
        check(act.get("type") == "activity_state" and act.get("activity") == "-1" and act.get("state") == 0,
              "activity_state idle after the earlier power off", str(act))
        check(first.get("bt_status", {}).get("type") == "bt_status" or first.get("bt_status") is None,
              "bt_status is the last payload or null", str(first.get("bt_status")))
        check(first.get("rf_status", {}).get("type") == "rf_status", "rf_status is the last pushed payload", str(first.get("rf_status")))
        files = first.get("config", {}).get("files", {})
        check(all(isinstance(files.get(k), int) for k in ("devices", "activities", "settings", "remote_map")),
              "config.files mtimes", str(files))
        check(files.get("devices", 0) > 0 and files.get("remote_map", 0) > 0, "mtimes of present files are set", str(files))
        check(isinstance(first.get("seq"), int) and isinstance(first.get("ts"), int), "seq and ts", str(first))
        check("msgId" not in first, "no msgId on the initial snapshot")
    # get_snapshot with msgId
    ws.send_json({"action": "get_snapshot", "msgId": 77})
    snap, frames = ws.wait_for("snapshot", 5, lambda m: m.get("msgId") == 77)
    check(snap is not None, "get_snapshot answered with msgId 77", str(frames))
    if snap and first:
        check(snap.get("seq") >= first.get("seq"), "seq did not go back")
    # the snapshot follows the state: switch, then get_snapshot shows running 11
    st, _ = hub.http_post("/api/activity", {"activity": "11"})
    ws.wait_for("activity_state", 10, lambda m: m.get("state") == 2 and m.get("activity") == "11")
    ws.send_json({"action": "get_snapshot", "msgId": 78})
    snap, _ = ws.wait_for("snapshot", 5, lambda m: m.get("msgId") == 78)
    check(snap is not None and snap.get("activity_state", {}).get("activity") == "11", "snapshot carries the last activity_state", str(snap))
    hub.http_post("/api/poweroff", {})
    ws.wait_for("activity_state", 10, lambda m: m.get("state") == 0)
    # a second client's first frame is also a snapshot, while the first keeps getting events
    ws2 = hub.ws()
    f2 = ws2.recv_json(5)
    check(f2 is not None and f2.get("type") == "snapshot" and f2.get("activity_state", {}).get("activity") == "-1",
          "second client: snapshot first, idle", str(f2))
    ws2.close()
    ws.close()


def signed_close(got, want, tol=15):
    """got: signed event timings; want: unsigned mark/space durations ending with a space"""
    if len(got) != len(want):
        return False
    for i, (g, w) in enumerate(zip(got, want)):
        if (g > 0) != (i % 2 == 0) or abs(abs(g) - w) > tol:
            return False
    return True


def test_ir_capture(hub):
    """IR capture mode (spec 3.14): the mock library plays <mock>/i2s_rx as the Hub's receiver"""
    rx = os.path.join(hub.mock_dir, "i2s_rx")
    if os.path.exists(rx):
        os.remove(rx)
    st, r = hub.http_post("/api/ir-capture", {"enabled": True})
    check(st == 503 and r.get("ir_capture") is False, "no receiver: 503, capture stays off", "%s %s" % (st, r))
    st, r = hub.http_post("/api/ir-capture", {"enabled": "yes"})
    check(st == 400, "enabled must be a boolean: 400", "%s %s" % (st, r))
    st, s = hub.http_get("/api/status")
    check(st == 200 and s.get("ir_capture") is False and "ir_capture" in s.get("capabilities", []),
          "status: ir_capture false at start, capability listed", str(s.get("ir_capture")))

    with open(rx, "wb") as f:
        f.write(b"\xff" * 4096)   # the line is idle
    ws = hub.ws()
    snap = ws.recv_json(5)
    check(snap is not None and (snap.get("hub") or {}).get("ir_capture") is False, "snapshot hub: ir_capture false",
          str((snap or {}).get("hub")))
    st, r = hub.http_post("/api/ir-capture", {"enabled": True})
    check(st == 200 and r.get("ok") is True and r.get("ir_capture") is True, "POST /api/ir-capture enabled: 200", "%s %s" % (st, r))
    st, s = hub.http_get("/api/status")
    check(s.get("ir_capture") is True, "status: ir_capture true", str(s.get("ir_capture")))
    ev, _ = ws.wait_for("ir_capture", 5)
    check(ev is not None and ev.get("ir_capture") is True and "ir_capture_error" not in ev and ev.get("seq"),
          "ir_capture event pushed: on", str(ev))
    ws.send_json({"action": "get_snapshot", "msgId": 30})
    snap, _ = ws.wait_for("snapshot", 5, lambda m: m.get("msgId") == 30)
    check(snap is not None and snap["hub"].get("ir_capture") is True and "ir_capture_error" not in snap["hub"],
          "snapshot hub: ir_capture true, no error", str((snap or {}).get("hub")))
    st, r = hub.http_post("/api/capture", {"timeout": 1})
    check(st == 409 and "capture mode" in str(r), "one-shot /api/capture while the mode is on: 409", "%s %s" % (st, r))

    # one press: an NEC frame, then the line goes quiet
    nec = nec_timings(0x20DF10EF)
    with open(rx, "ab") as f:
        f.write(ir_rx_bitstream(nec, idle_before_bytes=1024, idle_after_us=200000))
    msg, frames = ws.wait_for("ir_received", 5)
    check(msg is not None, "ir_received arrives", str(frames)[:300])
    if msg:
        t = msg.get("timings", [])
        check(37500 <= (msg.get("modulation") or 0) <= 38500, "modulation is the measured carrier (%s)" % msg.get("modulation"))
        # one frame (no second one seen) ends with the default 40 ms gap
        check(signed_close(t, nec + [40000]), "timings: the NEC frame, signed (+mark/-space), within 15 us",
              "%d values %s" % (len(t), t[:8]))
        check(isinstance(msg.get("seq"), int) and isinstance(msg.get("ts"), int), "seq and ts", str(msg)[:200])
    extra = [m for m in ws.drain(0.6) if m.get("type") == "ir_received"]
    check(extra == [], "one press gives one event", str(extra)[:200])

    # a send while the mode is on: it is not blocked, and what the receiver reads meanwhile (on a
    # Hub, the Hub's own blaster) is dropped. The send runs for real through the mock transmitter,
    # whose drain takes <mock>/i2s_tx_ms like a long signal going out.
    hub.http_post("/api/settings", {"dry_run": False})
    tx_ms = os.path.join(hub.mock_dir, "i2s_tx_ms")
    with open(tx_ms, "w") as f:
        f.write("800")
    res = {}

    def send():
        t0 = time.time()
        res["r"] = hub.http_post("/api/ir-raw", {"timings": [9000, -4500, 560, -560, 560], "frequency": 38000}, timeout=10)
        res["took"] = time.time() - t0

    th = threading.Thread(target=send)
    th.start()
    time.sleep(0.3)
    with open(rx, "ab") as f:
        f.write(ir_rx_bitstream(nec, idle_before_bytes=1024, idle_after_us=200000))   # light during the send
    th.join()
    os.remove(tx_ms)
    st, r = res.get("r", (0, {}))
    check(st == 200 and res.get("took", 0) >= 0.8, "ir-raw while capturing: 200 after the whole send (%.2f s)" % res.get("took", 0), "%s %s" % (st, r))
    leaked = [m for m in ws.drain(0.6) if m.get("type") == "ir_received"]
    check(leaked == [], "light read during a send gives no event", str(leaked)[:200])
    time.sleep(0.2)   # the capture also drops 100 ms after a send
    sirc = sirc12_timings([1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0])
    with open(rx, "ab") as f:
        f.write(ir_rx_bitstream(sirc, idle_before_bytes=1024, idle_after_us=200000))
    msg, frames = ws.wait_for("ir_received", 5)
    check(msg is not None and signed_close(msg.get("timings", []), sirc + [40000]),
          "after the send, the next press arrives (Sony 12-bit)", str(msg)[:300])
    hub.http_post("/api/settings", {"dry_run": True})

    # the WebSocket action, as on MQTT (ir_capture with enabled)
    ws.send_json({"action": "ir_capture", "enabled": False, "msgId": 31})
    ack, frames = ws.wait_for("ack", 5, lambda m: m.get("msgId") == 31)
    check(ack is not None and ack.get("ok") is True and ack.get("action") == "ir_capture" and ack.get("enabled") is False,
          "ws action ir_capture off: ack", str(ack))
    st, s = hub.http_get("/api/status")
    check(s.get("ir_capture") is False, "status: ir_capture false after off", str(s.get("ir_capture")))
    ev = next((f for f in frames if f.get("type") == "ir_capture"), None)   # may come before the ack
    if ev is None:
        ev, _ = ws.wait_for("ir_capture", 5)
    check(ev is not None and ev.get("ir_capture") is False, "ir_capture event pushed: off", str(ev))
    ws.send_json({"action": "ir_capture", "enabled": True, "msgId": 32})
    ack, _ = ws.wait_for("ack", 5, lambda m: m.get("msgId") == 32)
    check(ack is not None and ack.get("ok") is True and ack.get("enabled") is True, "ws action ir_capture on: ack", str(ack))
    ws.send_json({"action": "ir_capture", "msgId": 33})
    ack, _ = ws.wait_for("ack", 5, lambda m: m.get("msgId") == 33)
    check(ack is not None and ack.get("ok") is False and ack.get("code") == 400, "ws action without enabled: 400", str(ack))
    st, r = hub.http_post("/api/ir-capture", {"enabled": False})
    check(st == 200 and r.get("ir_capture") is False, "POST /api/ir-capture off: 200", "%s %s" % (st, r))
    with open(rx, "wb") as f:
        f.write(b"\xff" * 4096)   # the mock replays the file from the start on every open
    st, r = hub.http_post("/api/capture", {"timeout": 1})
    check(st == 408, "one-shot capture works again with the mode off (408, nothing pressed)", "%s %s" % (st, r))
    ws.close()
    os.remove(rx)


def main():
    with HubHarness(port=PORT) as hub:
        test_activity_switch(hub)
        test_state_watcher(hub)
        test_button_events(hub)
        test_rf_status_push(hub)
        test_snapshot(hub)
        test_ir_capture(hub)
    print("\n%d passed, %d failed" % (g_passed, g_failed))
    return 1 if g_failed else 0


if __name__ == "__main__":
    sys.exit(main())
