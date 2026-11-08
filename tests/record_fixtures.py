#!/usr/bin/env python3
"""
Records the fixtures the Home Assistant integration tests against (spec section 7), from the
host build with the sample configuration, so they can be refreshed after a change:

  tests/fixtures/ha/status.json, inventory.json, activities.json, rf_status.json, bt_status.json,
  snapshot.json, ws_activity_switch.ndjson (every frame a client received during a dry-run
  activity start and power off), ws_button.ndjson, ws_config_changed.ndjson, ir_raw_request.json

Usage: python3 tests/record_fixtures.py [--out DIR]
No private data: the daemon runs on a temporary mock root with tests/fixtures/ha/sample_config
and a synthetic mfg blob (unit id 20140101120000-000001); volatile values (ts, uptime, seq,
mtimes, ip) are kept as recorded.
"""

import argparse
import json
import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(TESTS_DIR, "fixtures", "ha")
PORT = 8991
UNIT_ID = "20140101120000-000001"
BT_STATUS_FILE = "/tmp/hub_btstack_status.json"

# hub_btstack's own format (/tmp/hub_btstack_status.json); hub_daemon adds ok, running, connected
# and pairing. "pid" is filled in at run time with a live process.
BT_STATUS = {
    "backend": "btstack", "pid": 0, "state": "connecting", "target": "11:22:33:44:55:66",
    "last_button": "", "last_seen": 0,
    "remote": {"connected": True, "addr": "11:22:33:44:55:66"},
    "host": {"connected": True, "addr": "AA:BB:CC:DD:EE:FF", "name": "Media Player", "pairing": False},
    "hosts": [{"addr": "AA:BB:CC:DD:EE:01", "name": "Television", "connected": False, "paired": True, "cid": 2},
              {"addr": "AA:BB:CC:DD:EE:FF", "name": "Media Player", "connected": True, "paired": True, "cid": 6}],
}

RF_STATUS = {
    "running": True, "ok": True, "fw_version": "v72.00", "paired": True, "rf_address": "A1B2C3D4",
    "hub_rf_address": "A1B2C3D4", "pairing_active": False, "sync_pending": False, "sync_pending_slot": 0,
    "sync_pending_diff": "Hub configuration changed. Push settings to sync.", "hub_config_version": 7,
    "ota": {"in_progress": False, "slot": 0, "status": 0, "sent_bytes": 0, "total_bytes": 0, "sent_chunks": 0,
            "total_chunks": 0, "error": ""},
    "slots": [
        {"slot": 1, "name": "Harmony Remote (Slot 1)", "model": "elite", "has_screen": True, "supports_ota": True,
         "active": True, "in_sync": True, "address": "E5F60718", "rf_address": "E5F60718", "sw_version": "2.2.0",
         "remote_id": "20141001120000-000002", "config_version": 7,
         "battery": {"percent": 87, "charging": False, "voltage_mv": 3980},
         "link": {"frames_rx": 120, "acked_sent": 12, "acked_ok": 12, "retries": 0, "naks": 0, "last_seen_s": 2}},
        {"slot": 2, "name": "Harmony Elite (Slot 2)", "model": "unknown", "has_screen": True, "supports_ota": True,
         "active": False, "in_sync": False, "address": "None", "rf_address": "None", "sw_version": "unknown",
         "config_version": 0, "battery": {"percent": 0, "charging": False, "voltage_mv": 0},
         "link": {"frames_rx": 0, "acked_sent": 0, "acked_ok": 0, "retries": 0, "naks": 0, "last_seen_s": -1}},
    ],
}

IR_RAW_REQUEST = {"frequency": 38000, "timings": [9000, -4500, 560, -560, 560, -1690, 560, -560, 560, -39000],
                  "port": 1, "repeats": 1}


def rec(s):
    b = s.encode("ascii") if isinstance(s, str) else s
    return struct.pack("!I", len(b)) + b


def mfg_blob(unit_id):
    recs = b"harmony!" + rec("unitid") + rec(unit_id) + rec("arch") + rec("0x11") + rec("skin") + rec("0x61") \
        + rec("usb_vendor_id") + rec("0x046D") + rec("hw_ver") + rec("01.00") + rec("key") + rec(bytes(range(256)) * 4)
    img = bytearray(b"\xff" * 0x10000)
    img[0x9000:0x9000 + len(recs)] = recs
    return bytes(img)


def write_json(out_dir, name, obj):
    with open(os.path.join(out_dir, name), "w") as f:
        json.dump(obj, f, indent=2, sort_keys=False)
        f.write("\n")
    print("  wrote %s" % name)


def write_ndjson(out_dir, name, frames):
    with open(os.path.join(out_dir, name), "w") as f:
        for fr in frames:
            f.write(json.dumps(fr, sort_keys=False) + "\n")
    print("  wrote %s (%d frames)" % (name, len(frames)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=OUT_DIR)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    with tempfile.NamedTemporaryFile(prefix="mfg_", suffix=".bin", delete=False) as f:
        f.write(mfg_blob(UNIT_ID))
        mfg_path = f.name
    tmp = BT_STATUS_FILE + ".tmp"
    try:
        with open(tmp, "w") as f:
            json.dump(dict(BT_STATUS, pid=os.getpid()), f)
        os.replace(tmp, BT_STATUS_FILE)
        print("[-] Recording milestone 1 fixtures into %s" % args.out)
        with HubHarness(port=PORT, args=["--mfg-file", mfg_path]) as hub:
            time.sleep(1.5)  # watcher baseline (bt_status emitted from the file)
            st, r = hub.http_post("/api/rf/status-changed", RF_STATUS)
            assert st == 200, (st, r)
            time.sleep(0.3)

            for name, path in (("status.json", "/api/status"), ("inventory.json", "/api/inventory"),
                               ("activities.json", "/api/activities"), ("bt_status.json", "/api/bt-status")):
                st, body = hub.http_get(path)
                assert st == 200, (path, st, body)
                write_json(args.out, name, body)
            # rf_status: what the daemon's proxy would return is hub_rf's object; record the pushed one
            write_json(args.out, "rf_status.json", RF_STATUS)

            ws = hub.ws()
            snap = ws.recv_json(5)
            assert snap and snap.get("type") == "snapshot", snap
            write_json(args.out, "snapshot.json", snap)

            # activity switch: start 10, wait for running, power off, wait for idle
            frames = []
            st, _ = hub.http_post("/api/activity", {"activity": "10"})
            m, seen = ws.wait_for("activity_state", 15, lambda x: x.get("state") == 2 and x.get("activity") == "10")
            frames += seen
            st, _ = hub.http_post("/api/poweroff", {})
            m, seen = ws.wait_for("activity_state", 15, lambda x: x.get("state") == 0)
            frames += seen
            frames += ws.drain(0.5)
            write_ndjson(args.out, "ws_activity_switch.ndjson", frames)

            # button press and release
            for action in ("press", "release"):
                hub.http_post("/api/button-press", {"button": "VolumeUp", "action": action, "source": "remote_rf",
                                                    "slot": 1, "deviceId": "1", "command": "VolumeUp"})
            time.sleep(0.3)
            frames = [f for f in ws.drain(0.5) if f.get("type") == "button"]
            write_ndjson(args.out, "ws_button.ndjson", frames)

            # config change: newer mtime on devices.json
            devices = os.path.join(hub.data_dir, "config", "devices.json")
            os.utime(devices, (time.time() + 5, time.time() + 5))
            m, seen = ws.wait_for("config_changed", 5)
            write_ndjson(args.out, "ws_config_changed.ndjson", [f for f in seen if f.get("type") == "config_changed"])
            ws.close()

            write_json(args.out, "ir_raw_request.json", IR_RAW_REQUEST)
            st, r = hub.http_post("/api/ir-raw", IR_RAW_REQUEST)
            assert st == 200, (st, r)
            write_json(args.out, "ir_raw_response.json", r)
    finally:
        for p in (tmp, BT_STATUS_FILE, mfg_path):
            try:
                os.remove(p)
            except OSError:
                pass
    print("[+] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
