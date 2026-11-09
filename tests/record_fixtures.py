#!/usr/bin/env python3
"""
Records the fixtures the Home Assistant integration tests against (spec section 7), from the
host build with the sample configuration, so they can be refreshed after a change:

  tests/fixtures/ha/status.json, inventory.json, activities.json, rf_status.json, bt_status.json,
  snapshot.json, ws_activity_switch.ndjson (every frame a client received during a dry-run
  activity start and power off), ws_button.ndjson, ws_config_changed.ndjson, ir_raw_request.json;
  spec 3.14: ir_capture_response.json, ws_ir_capture.ndjson (on and off), ws_ir_received.ndjson (the real Hub recording
  tests/fixtures/ir/samsung36_volume_up.i2s played as the IR receiver);
  milestone 3: mqtt_retained.json (topic to payload after connect), mqtt_activity_switch.ndjson
  (topic and payload per message during a dry-run activity start and power off), mqtt_cmd_response.json

Usage: python3 tests/record_fixtures.py [--out DIR]
No private data: the daemon runs on a temporary mock root with tests/fixtures/ha/sample_config
and a synthetic mfg blob (unit id 20140101120000-000001); volatile values (ts, uptime, seq,
mtimes, ip) are kept as recorded.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG, REPO_ROOT  # noqa: E402
from mock_mqtt import MockBroker, MockClient  # noqa: E402
import bt_status_seed  # noqa: E402

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(TESTS_DIR, "fixtures", "ha")
PORT = 8991
UNIT_ID = "20140101120000-000001"
BT_STATUS_FILE = "/tmp/hub_btstack_status.json"


def load_rf_status():
    """hub_rf's RF status object from the real builder (tests/rf_status_sample.c: a Harmony
    Ultimate in slot 1 with skin 0x64 and EquadID 0x4021, slot 2 empty; dummy addresses)"""
    src = os.path.join(REPO_ROOT, "payload", "source")
    out = os.path.join(REPO_ROOT, "build", "tmp", "test_host", "rf_status_sample")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run(["gcc", "-Wall", "-O2", "-I" + src, os.path.join(REPO_ROOT, "tests", "rf_status_sample.c"),
                    os.path.join(src, "rf_state.c"), os.path.join(src, "rf_api.c"), os.path.join(src, "cJSON.c"),
                    "-lpthread", "-lm", "-o", out], check=True)
    with open(os.path.join(REPO_ROOT, "VERSION")) as f:
        version = f.read().strip()
    res = subprocess.run([out, version], check=True, capture_output=True, text=True)
    return json.loads(res.stdout)


RF_STATUS = load_rf_status()

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


DOC_IP = "192.0.2.10"  # RFC 5737 documentation address


def host_addresses():
    """The recording host's IPv4 addresses (not loopback): they must not end up in a fixture"""
    import socket
    found = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))  # nothing is sent: picks the source address of the default route
        found.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            found.add(info[4][0])
    except OSError:
        pass
    return [a for a in found if a and not a.startswith("127.")]


def scrub_dir(out_dir):
    """Replaces the recording host's addresses with DOC_IP in every fixture written"""
    addrs = host_addresses()
    if not addrs:
        return
    for name in sorted(os.listdir(out_dir)):
        path = os.path.join(out_dir, name)
        if not os.path.isfile(path):
            continue
        with open(path, newline="") as f:
            text = f.read()
        new = text
        for a in addrs:
            new = re.sub(r"(?<![0-9.])" + re.escape(a) + r"(?![0-9])", DOC_IP, new)
        if new != text:
            with open(path, "w", newline="") as f:
                f.write(new)
            print("  scrubbed the host address in %s" % name)


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
        # hub_btstack's file from its real writer (tests/bt_status_sample.c); pid 1 is always
        # alive, so "running" is true and the fixtures do not change with the recorder's pid
        bt_status_seed.write_status(BT_STATUS_FILE, pid=1)
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

            # IR capture mode (3.14): a real Hub recording (Samsung soundbar remote, Volume Up)
            # played as the mock's IR receiver
            rx = os.path.join(hub.mock_dir, "i2s_rx")
            with open(rx, "wb") as f:
                f.write(b"\xff" * 4096)
            st, r = hub.http_post("/api/ir-capture", {"enabled": True})
            assert st == 200, (st, r)
            write_json(args.out, "ir_capture_response.json", r)
            ev_on, _ = ws.wait_for("ir_capture", 5)
            assert ev_on is not None and ev_on.get("ir_capture") is True, ev_on
            with open(rx, "ab") as f, open(os.path.join(REPO_ROOT, "tests", "fixtures", "ir", "samsung36_volume_up.i2s"), "rb") as g:
                f.write(g.read())
                f.write(b"\xff" * 30000)
            m, seen = ws.wait_for("ir_received", 5)
            assert m is not None, seen
            write_ndjson(args.out, "ws_ir_received.ndjson", [m])
            st, r = hub.http_post("/api/ir-capture", {"enabled": False})
            assert st == 200, (st, r)
            ev_off, _ = ws.wait_for("ir_capture", 5)
            assert ev_off is not None and ev_off.get("ir_capture") is False, ev_off
            write_ndjson(args.out, "ws_ir_capture.ndjson", [ev_on, ev_off])
            ws.close()

            write_json(args.out, "ir_raw_request.json", IR_RAW_REQUEST)
            st, r = hub.http_post("/api/ir-raw", IR_RAW_REQUEST)
            assert st == 200, (st, r)
            write_json(args.out, "ir_raw_response.json", r)

        # Milestone 3: MQTT v2 against the mock broker
        print("[-] Recording MQTT fixtures")
        broker = MockBroker()
        mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": broker.port, "baseTopic": "harmony/hub",
                    "clientId": "harmony_hub"}
        try:
            with HubHarness(port=PORT + 1, args=["--mfg-file", mfg_path], env={"HARMONY_MQTT_LIVE": "1"},
                            extra_data_files={"mqtt.json": mqtt_cfg}) as hub:
                obs = MockClient("127.0.0.1", broker.port, "observer")
                obs.subscribe("harmony/hub/#")
                time.sleep(1.5)
                hub.http_post("/api/rf/status-changed", RF_STATUS)
                deadline = time.time() + 10
                while time.time() < deadline and "harmony/hub/rf" not in broker.retained:
                    time.sleep(0.1)
                time.sleep(0.5)
                retained = {}
                for topic, payload in sorted(broker.retained.items()):
                    try:
                        retained[topic] = json.loads(payload.decode("utf-8"))
                    except ValueError:
                        retained[topic] = payload.decode("utf-8", "replace")
                write_json(args.out, "mqtt_retained.json", retained)
                obs.drain()
                hub.http_post("/api/activity", {"activity": "10"})
                obs.wait_for("harmony/hub/activity_state", 15, lambda p: json.loads(p).get("state") == 2)
                hub.http_post("/api/poweroff", {})
                obs.wait_for("harmony/hub/activity_state", 15, lambda p: json.loads(p).get("state") == 0)
                time.sleep(0.5)
                lines = []
                for topic, payload, retain in obs.drain():
                    try:
                        body = json.loads(payload.decode("utf-8"))
                    except ValueError:
                        body = payload.decode("utf-8", "replace")
                    lines.append({"topic": topic, "payload": body})
                write_ndjson(args.out, "mqtt_activity_switch.ndjson", lines)
                cmd = MockClient("127.0.0.1", broker.port, "commander")
                cmd.subscribe("harmony/hub/cmd/response")
                cmd.publish("harmony/hub/cmd", json.dumps({"id": "c7d1", "action": "start_activity", "activity": "10"}), qos=1)
                m = cmd.wait_for("harmony/hub/cmd/response", 5, lambda p: json.loads(p).get("id") == "c7d1")
                cmd.publish("harmony/hub/cmd", json.dumps({"id": "c7d2", "action": "start_activity", "activity": "11"}), qos=1)
                m2 = cmd.wait_for("harmony/hub/cmd/response", 5, lambda p: json.loads(p).get("id") == "c7d2")
                write_json(args.out, "mqtt_cmd_response.json", {
                    "request": {"id": "c7d1", "action": "start_activity", "activity": "10"},
                    "response": json.loads(m[1]) if m else None,
                    "queued_request": {"id": "c7d2", "action": "start_activity", "activity": "11"},
                    "queued_response": json.loads(m2[1]) if m2 else None,
                })
                cmd.close()
                obs.close()
        finally:
            broker.close()

        # Milestone 4: the Logitech protocol shim and SSDP
        print("[-] Recording Logitech shim fixtures")
        import test_hbus
        import test_ssdp
        test_hbus.LEGACY_PORT = PORT + 100
        test_ssdp.SSDP_PORT = PORT + 200
        with open(os.path.join(SAMPLE_CONFIG, "settings.json")) as f:
            legacy_settings = json.load(f)
        legacy_settings["legacy_enabled"] = True
        with HubHarness(port=PORT + 2, args=["--mfg-file", mfg_path, "--legacy-port", str(test_hbus.LEGACY_PORT),
                                             "--ssdp-port", str(test_ssdp.SSDP_PORT)],
                        extra_config_files={"settings.json": legacy_settings},
                        extra_root_files={"luaworks/provision/preferences": {"remoteId": 1234567}}) as hub:
            time.sleep(0.5)
            st, head, prov = test_hbus.provision(hub)
            write_json(args.out, "hbus_provision.json", prov)
            # the LOCATION carries the recording host's address and the test port: documented values instead
            ssdp = test_ssdp.msearch("urn:myharmony-com:device:harmony:1")
            ssdp = re.sub(r"LOCATION: http://[0-9.]+:[0-9]+/", "LOCATION: http://192.0.2.10:8088/", ssdp)
            with open(os.path.join(args.out, "ssdp_response.txt"), "w") as f:
                f.write(ssdp)
            st, head, xml = test_hbus.http_get("/description")
            with open(os.path.join(args.out, "description.xml"), "w") as f:
                f.write(xml)
            ws = test_hbus.legacy_ws(1234567)
            frames = []
            first, _ = test_hbus.recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify", 3.0)
            frames.append(first)
            rid = test_hbus.hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?config")
            r, _ = test_hbus.reply_for(ws, rid)
            write_json(args.out, "hbus_config.json", r)
            rid = test_hbus.hbus(ws, "harmony.activityengine?runactivity", {"async": "true", "timestamp": 0, "activityId": "10"})
            m, seen = test_hbus.recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify" and m["data"].get("activityStatus") == 2, 15.0)
            frames += seen + ([m] if m else [])
            rid = test_hbus.hbus(ws, "harmony.activityengine?runactivity", {"async": "true", "timestamp": 0, "activityId": "-1"})
            m, seen = test_hbus.recv_until(ws, lambda m: m.get("type") == "harmony.engine?startActivityFinished", 15.0)
            frames += seen + ([m] if m else [])
            m, seen = test_hbus.recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify", 5.0)
            frames += seen + ([m] if m else [])
            write_ndjson(args.out, "hbus_activity_switch.ndjson", [f for f in frames if f])
            ws.close()
    finally:
        for p in (tmp, BT_STATUS_FILE, mfg_path):
            try:
                os.remove(p)
            except OSError:
                pass
    scrub_dir(args.out)
    print("[+] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
