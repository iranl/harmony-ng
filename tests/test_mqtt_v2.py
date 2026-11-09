#!/usr/bin/env python3
"""
MQTT v2 (spec section 5) against hub_daemon_test connected to the stdlib mock broker
(tests/mock_mqtt.py), publishing for real through HARMONY_MQTT_LIVE=1: retained topics after
connect, the one-time clearing of the old state and discovery topics, activity_state and progress
on a dry-run switch, bt/rf retained from pushes, inventory chunks and inventory_version, the
command channel (cmd_response with the same id, 409 busy, get_snapshot, malformed JSON, no id = no
reply) and ir/raw reaching the recorded encoder call.
"""

import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, WsClient  # noqa: E402
from ir_rx_signal import ir_rx_bitstream, nec_timings  # noqa: E402
import bt_status_seed  # noqa: E402
from mock_mqtt import MockBroker, MockClient  # noqa: E402

PORT = 8989
BASE = "harmony/hub"
UNIT_ID = "20140101120000-000001"
BT_STATUS_FILE = "/tmp/hub_btstack_status.json"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def rec(s):
    b = s.encode("ascii") if isinstance(s, str) else s
    return struct.pack("!I", len(b)) + b


def mfg_blob(unit_id):
    recs = b"harmony!" + rec("unitid") + rec(unit_id) + rec("arch") + rec("0x11")
    img = bytearray(b"\xff" * 0x10000)
    img[0x9000:0x9000 + len(recs)] = recs
    return bytes(img)


def retained_json(broker, topic):
    p = broker.retained.get(topic)
    return json.loads(p.decode("utf-8")) if p else None


def jload(payload):
    try:
        return json.loads(payload.decode("utf-8"))
    except ValueError:
        return None


def main():
    import tempfile
    broker = MockBroker()
    with tempfile.NamedTemporaryFile(prefix="mfg_", suffix=".bin", delete=False) as f:
        f.write(mfg_blob(UNIT_ID))
        mfg_path = f.name
    mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": broker.port, "baseTopic": BASE,
                "clientId": "harmony-test", "discoveryPrefix": "homeassistant",
                "slot1RemoteEvents": True, "haDiscovery": True}
    try:
        print("[-] 5.1/5.2 retained topics after connect, old topics cleared")
        with HubHarness(port=PORT, args=["--mfg-file", mfg_path], env={"HARMONY_MQTT_LIVE": "1"},
                        extra_data_files={"mqtt.json": mqtt_cfg}) as hub:
            sub = MockClient("127.0.0.1", broker.port, "observer")
            sub.subscribe(BASE + "/#")
            sub.subscribe("homeassistant/#")
            # wait for the daemon to connect and publish
            deadline = time.time() + 10
            while time.time() < deadline and (BASE + "/inventory/index") not in broker.retained:
                time.sleep(0.1)
            r = broker.retained
            check(r.get(BASE + "/status") == b"online", "status online retained", str(r.get(BASE + "/status")))
            ws = WsClient("127.0.0.1", PORT)
            snap = ws.recv_json(timeout=5)
            mst = (snap or {}).get("mqtt_status") or {}
            check(mst.get("type") == "mqtt_status" and mst.get("connected") is True and mst.get("enabled") is True
                  and mst.get("broker") == "127.0.0.1" and mst.get("baseTopic") == BASE,
                  "snapshot carries mqtt_status connected", str(mst))
            hubo = retained_json(broker, BASE + "/hub")
            check(hubo is not None and hubo.get("type") == "hub" and hubo.get("hub_id") == UNIT_ID
                  and "mqtt_v2" in hubo.get("capabilities", []) and "ts" in hubo, "hub retained with hub_id and capabilities", str(hubo))
            act = retained_json(broker, BASE + "/activity_state")
            check(act is not None and act.get("type") == "activity_state" and act.get("activity") == "-1", "activity_state retained (idle)", str(act))
            cfg = retained_json(broker, BASE + "/config")
            check(cfg is not None and cfg.get("type") == "config" and len(cfg.get("inventory_version", "")) == 8
                  and set(cfg.get("files", {}).keys()) >= {"devices", "activities", "settings", "remote_map"},
                  "config retained with mtimes and inventory_version", str(cfg))
            idx = retained_json(broker, BASE + "/inventory/index")
            check(idx is not None and idx.get("type") == "inventory_index" and idx.get("inventory_version") == cfg.get("inventory_version")
                  and [d["id"] for d in idx.get("devices", [])] == ["1", "2"] and [a["id"] for a in idx.get("activities", [])] == ["10", "11"],
                  "inventory/index lists the sample devices and activities", str(idx))
            d1 = retained_json(broker, BASE + "/inventory/1")
            check(d1 is not None and d1.get("type") == "inventory_device" and d1.get("name") == "Living Room TV"
                  and d1.get("commands") == ["PowerOn", "PowerOff", "InputHdmi1", "VolumeUp", "VolumeDown"]
                  and "raw" not in json.dumps(d1), "inventory/1: names only, no codes", str(d1))
            check(d1 is not None and d1.get("deviceType") == "Television" and d1.get("controlPort") == 7, "device chunk fields", str(d1))
            # old topics cleared once: empty retained publishes in the broker log
            cleared = {t for (t, p, rt, q) in broker.log if rt and p == b""}
            for t in ("state", "activity", "current_activity", "activity_id"):
                check((BASE + "/" + t) in cleared, "old topic %s cleared" % t, str(sorted(cleared))[:200])
            marker = os.path.join(hub.data_dir, "mqtt_v2_migrated")
            check(os.path.exists(marker), "marker file written once")
            # nothing old is published or subscribed any more
            published = {t for (t, p, rt, q) in broker.log if p}
            check(not any(t.endswith("/state") or t.endswith("/activity_id") for t in published), "no old state payloads", str(published))
            check(hubo is not None and hubo.get("device_count") == 2 and hubo.get("command_count") == 8 and "ip" in hubo,
                  "hub carries ip, device_count and command_count", str(hubo))

            print("[-] 5.4 Home Assistant discovery on the v2 topics")
            sel = retained_json(broker, "homeassistant/select/harmony-test_activity/config")
            check(sel is not None and sel.get("unique_id") == "harmony-test_activity" and sel.get("state_topic") == BASE + "/activity_state"
                  and sel.get("command_topic") == BASE + "/activity/set" and sel.get("options") == ["PowerOff", "Watch TV", "Media Player"]
                  and "'10': 'Watch TV'" in sel.get("value_template", "") and "value_json.activity" in sel.get("value_template", ""),
                  "select Activity: v2 state topic, alias command topic, id to name template", str(sel)[:300])
            dev = (sel or {}).get("device", {})
            check(dev.get("identifiers") == ["harmony-test", "harmony-ng-" + UNIT_ID] and dev.get("name") == "Test Hub"
                  and dev.get("manufacturer") == "Logitech" and dev.get("configuration_url", "").endswith(":8089/"),
                  "device object: old identifier kept, hub id added", str(dev))
            check(sel is not None and sel.get("availability_topic") == BASE + "/status", "availability on <base>/status")
            aid = retained_json(broker, "homeassistant/sensor/harmony-test_activity_id/config")
            check(aid is not None and aid.get("state_topic") == BASE + "/activity_state" and aid.get("value_template") == "{{ value_json.activity }}",
                  "sensor Activity ID reads activity_state", str(aid)[:200])
            for obj, key in (("ip", "ip"), ("firmware", "version"), ("ir_devices", "device_count"), ("ir_commands", "command_count")):
                c = retained_json(broker, "homeassistant/sensor/harmony-test_%s/config" % obj)
                check(c is not None and c.get("state_topic") == BASE + "/hub" and ("value_json.%s" % key) in c.get("value_template", ""),
                      "sensor %s reads hub.%s" % (obj, key), str(c)[:200])
            po = retained_json(broker, "homeassistant/button/harmony-test_power_off/config")
            check(po is not None and po.get("command_topic") == BASE + "/activity/set" and po.get("payload_press") == "PowerOff", "button Power Off", str(po)[:200])
            sy = retained_json(broker, "homeassistant/button/harmony-test_sync/config")
            check(sy is not None and sy.get("command_topic") == BASE + "/sync", "button Sync", str(sy)[:200])
            sl = retained_json(broker, "homeassistant/button/harmony-test_remote_2_sleep/config")
            check(sl is not None and sl.get("command_topic") == BASE + "/remote/2/sleep", "button Remote 2 Sleep", str(sl)[:200])
            # the birth message republishes the configs
            n_before = len([1 for (t, p, rt, q) in broker.log if t == "homeassistant/select/harmony-test_activity/config" and p])
            birth = MockClient("127.0.0.1", broker.port, "ha-birth")
            sub.drain()  # the configs published at connect are still queued: wait for a new one
            birth.publish("homeassistant/status", "online", retain=False, qos=0)
            m = sub.wait_for("homeassistant/select/harmony-test_activity/config", 5)
            time.sleep(0.2)
            n_after = len([1 for (t, p, rt, q) in broker.log if t == "homeassistant/select/harmony-test_activity/config" and p])
            check(m is not None and n_after > n_before, "HA birth message: discovery republished", "%d -> %d" % (n_before, n_after))
            birth.close()

            print("[-] 5.2 events: activity switch, bt and rf pushes")
            sub.drain()
            st, _ = hub.http_post("/api/activity", {"activity": "10"})
            m = sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 2)
            check(m is not None and jload(m[1]).get("activity") == "10", "activity_state running published", str(m))
            prog = [x for x in sub.drain() if x[0] == BASE + "/progress"]
            check(len(prog) >= 2 and all(not x[2] for x in prog) and jload(prog[0][1]).get("type") == "activity_progress",
                  "progress frames, not retained (%d)" % len(prog), str(prog[:1]))
            check(retained_json(broker, BASE + "/activity_state").get("activity") == "10", "activity_state retained value follows")
            hub.http_post("/api/poweroff", {})
            sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 0)
            # rf push and bt file
            rf = {"running": True, "ok": True, "slots": [{"slot": 1, "active": True, "battery": {"percent": 50, "charging": False, "voltage_mv": 3700}, "link": {"frames_rx": 1}}]}
            hub.http_post("/api/rf/status-changed", rf)
            m = sub.wait_for(BASE + "/rf", 5)
            check(m is not None and jload(m[1]).get("type") == "rf_status" and jload(m[1])["slots"][0]["battery"]["percent"] == 50, "rf published and retained", str(m))
            bt_status_seed.write_status(BT_STATUS_FILE, host_connected=False)  # the real writer's format
            m = sub.wait_for(BASE + "/bt", 5)
            btp = jload(m[1]) if m else {}
            check(m is not None and btp.get("type") == "bt_status" and btp.get("running") is True
                  and btp.get("connected") is False and len(btp.get("hosts", [])) == 2,
                  "bt published (hub_btstack's fields plus running and connected)", str(m))
            # button event -> remote/<slot>/button, QoS 1, not retained
            sub.drain()
            hub.http_post("/api/button-press", {"button": "VolumeUp", "action": "press", "source": "remote_rf", "slot": 1, "deviceId": "1", "command": "VolumeUp"})
            m = sub.wait_for(BASE + "/remote/1/button", 5)
            check(m is not None and jload(m[1]).get("button") == "VolumeUp" and not m[2], "button on remote/1/button", str(m))
            qos1 = [q for (t, p, rt, q) in broker.log if t == BASE + "/remote/1/button"]
            check(qos1 and qos1[-1] == 1, "button published at QoS 1", str(qos1))

            print("[-] 5.1 inventory_version follows the device file")
            version_before = retained_json(broker, BASE + "/config")["inventory_version"]
            dev_path = os.path.join(hub.data_dir, "config", "devices.json")
            with open(dev_path) as f:
                devs = json.load(f)
            devs["devices"][0]["name"] = "Living Room TV 2"
            with open(dev_path + ".tmp", "w") as f:
                json.dump(devs, f)
            os.replace(dev_path + ".tmp", dev_path)
            m = sub.wait_for(BASE + "/inventory/index", 8, lambda p: (jload(p) or {}).get("inventory_version") != version_before)
            check(m is not None, "index republished with a new inventory_version", str(m))
            m2 = sub.wait_for(BASE + "/inventory/1", 5, lambda p: (jload(p) or {}).get("name") == "Living Room TV 2")
            check(m2 is not None, "changed device chunk republished", str(m2))
            check(retained_json(broker, BASE + "/config")["inventory_version"] != version_before, "config carries the new version")

            print("[-] 5.3 command channel")
            cmd = MockClient("127.0.0.1", broker.port, "commander")
            cmd.subscribe(BASE + "/cmd/response")
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c1", "action": "get_state"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c1")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("type") == "cmd_response" and r.get("ok") is True and r.get("action") == "get_state"
                  and r.get("activity_state", {}).get("activity") == "-1", "get_state answered with the id", str(r))
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c2", "action": "start_activity", "activity": "10"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c2")
            check(m is not None and jload(m[1]).get("ok") is True, "start_activity ok", str(m))
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c3", "action": "start_activity", "activity": "11"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c3")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("ok") is False and r.get("code") == 409 and r.get("error") == "busy", "busy start: 409", str(r))
            sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 2)
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c4", "action": "get_snapshot"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c4")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("snapshot", {}).get("type") == "snapshot" and r["snapshot"]["hub"]["hub_id"] == UNIT_ID,
                  "get_snapshot reply carries the snapshot", str(r)[:200])
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c5", "action": "no_such"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c5")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("code") == 400, "unknown action: 400", str(r))
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c7", "action": "start_activity"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c7")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("ok") is False and r.get("code") == 400,
                  "start_activity without activity: 400, the request id is not an activity", str(r))
            cmd.publish(BASE + "/cmd", '{"id":"c6","action":"ping",', qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c6")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("code") == 400 and "malformed" in r.get("error", ""), "malformed JSON with an id: 400", str(r))
            cmd.drain()
            cmd.publish(BASE + "/cmd", json.dumps({"action": "stop_activity"}), qos=1)
            m = sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 0)
            check(m is not None, "a request without id is executed (power off)")
            check(cmd.wait_for(BASE + "/cmd/response", 1.0) is None, "and gets no reply")
            # user commands through cmd
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c7", "action": "send_command", "deviceId": "1", "command": "VolumeUp"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c7")
            check(m is not None and jload(m[1]).get("ok") is True, "send_command ok (dry run)", str(m))
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c8", "action": "rf_wake", "slot": 1}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c8")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("ok") is False and r.get("code") == 503, "rf_wake without hub_rf: 503", str(r))

            print("[-] 5.3 aliases of the first MQTT layer")
            sub.drain()
            cmd.publish(BASE + "/activity/set", "Watch TV", qos=1)
            m = sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 2 and (jload(p) or {}).get("activity") == "10")
            check(m is not None, "activity/set with a name starts the activity", str(m))
            cmd.publish(BASE + "/activity/set", "PowerOff", qos=1)
            m = sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 0)
            check(m is not None, "activity/set PowerOff powers off")
            cmd.publish(BASE + "/activity/set", "11", qos=1)
            m = sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 2 and (jload(p) or {}).get("activity") == "11")
            check(m is not None, "activity/set with an id starts the activity")
            cmd.publish(BASE + "/activity/set", "-1", qos=1)
            sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 0)
            st, a = hub.http_get("/api/test/ir-raw-last")
            sub.drain()
            cmd.publish(BASE + "/send_command", json.dumps({"device": "Living Room TV", "command": "VolumeUp"}), qos=1)
            cmd.publish(BASE + "/command/1/VolumeDown", "1", qos=1)
            time.sleep(1.0)
            st, s_after = hub.http_get("/api/status")
            check(st == 200, "send_command and command/<dev>/<cmd> accepted (daemon alive)")
            sub.drain()
            cmd.publish(BASE + "/sync", "PRESS", qos=1)
            m = sub.wait_for(BASE + "/inventory/index", 5)
            m2 = sub.wait_for("homeassistant/select/harmony-test_activity/config", 5)
            check(m is not None and m2 is not None, "sync republishes the retained state and the discovery")
            cmd.publish(BASE + "/remote/1/sleep", "PRESS", qos=1)
            time.sleep(0.5)
            st, s_after = hub.http_get("/api/status")
            check(st == 200, "remote/1/sleep accepted without hub_rf (no reply, daemon alive)")

            print("[-] 5.6 MQTT-type devices: pulse on the device topic")
            sub.drain()
            st, r = hub.http_post("/api/device-mqtt-test", {"deviceId": "1", "device": "Living Room TV", "command": "PowerToggle",
                                                            "topic": "{base_topic}/button/{device}/{command}", "pulseMs": 300})
            check(st == 200 and r.get("ok") is True and r.get("topic") == BASE + "/button/Living Room TV/PowerToggle" and r.get("pulseMs") == 300,
                  "device-mqtt-test resolves the template", str(r))
            m = sub.wait_for(BASE + "/button/Living Room TV/PowerToggle", 5, lambda p: p == b"1")
            m0 = sub.wait_for(BASE + "/button/Living Room TV/PowerToggle", 5, lambda p: p == b"0")
            check(m is not None and m0 is not None, "pulse published: 1 then 0", str((m, m0)))
            st, r = hub.http_post("/api/device-save", {"deviceId": "3", "name": "Plug", "type": "MQTT", "mqttTopic": "/plug/{command}", "mqttPulseMs": 200})
            check(st == 200, "an MQTT device is saved with its topic", str(r))
            sub.drain()
            st, r = hub.http_post("/api/ir-send", {"deviceId": "3", "command": "PowerOn"})
            m = sub.wait_for(BASE + "/plug/PowerOn", 5, lambda p: p == b"1")
            m0 = sub.wait_for(BASE + "/plug/PowerOn", 5, lambda p: p == b"0")
            mb = sub.wait_for(BASE + "/button", 5)
            check(m is not None and m0 is not None, "a command to an MQTT device pulses its own topic", str((st, r)))
            check(mb is not None and jload(mb[1]).get("deviceId") == "3", "and raises the button event", str(mb))

            print("[-] 5.3 ir/raw")
            st, a = hub.http_get("/api/test/ir-raw-last")
            calls_before = a.get("calls", 0)
            cmd.publish(BASE + "/ir/raw", json.dumps({"id": "r1", "frequency": 38000, "timings": [9000, -4500, 560, -560], "port": 1}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "r1")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("ok") is True and r.get("action") == "ir_raw" and r.get("count") == 4, "ir/raw answered", str(r))
            st, b = hub.http_get("/api/test/ir-raw-last")
            check(b.get("calls") == calls_before + 1 and b.get("frequency") == 38000 and b.get("first") == [9000, 4500, 560, 560], "encoder call recorded", str(b))
            cmd.publish(BASE + "/ir/raw", json.dumps({"id": "r2", "timings": [1]}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "r2")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("code") == 400 and "timings" in r.get("error", ""), "ir/raw validation error", str(r))

            print("[-] 3.14 cmd ir_capture and ir/received")
            hubo = retained_json(broker, BASE + "/hub") or {}
            check(hubo.get("ir_capture") is False, "retained hub: ir_capture false", str(hubo.get("ir_capture")))
            rx = os.path.join(hub.mock_dir, "i2s_rx")
            with open(rx, "wb") as f:
                f.write(b"\xff" * 4096)   # the mock's IR receiver, idle
            time.sleep(3)   # the device saves above republish the hub object a moment later
            sub.drain()
            cmd.publish(BASE + "/cmd", json.dumps({"id": "i1", "action": "ir_capture", "enabled": True}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "i1")
            r = jload(m[1]) if m else {}
            check(r.get("ok") is True and r.get("action") == "ir_capture" and r.get("enabled") is True, "cmd ir_capture on answered", str(r))
            mh = sub.wait_for(BASE + "/hub", 5, lambda p: (jload(p) or {}).get("ir_capture") is True)
            check(mh is not None and (retained_json(broker, BASE + "/hub") or {}).get("ir_capture") is True,
                  "hub object republished (retained) with ir_capture true", str(mh))
            nec = nec_timings(0x20DF10EF)
            with open(rx, "ab") as f:
                f.write(ir_rx_bitstream(nec, idle_before_bytes=1024, idle_after_us=200000))
            mi = sub.wait_for(BASE + "/ir/received", 5)
            p = jload(mi[1]) if mi else {}
            t = p.get("timings", [])
            check(p.get("type") == "ir_received" and 37500 <= (p.get("modulation") or 0) <= 38500 and len(t) == len(nec) + 1
                  and t[0] > 0 and t[1] < 0, "ir/received carries the signal", str(p)[:200])
            pubs = [(rt, q) for (tp, pl, rt, q) in broker.log if tp == BASE + "/ir/received"]
            check(pubs and all(rt is False or rt == 0 for rt, q in pubs) and all(q == 0 for rt, q in pubs)
                  and (BASE + "/ir/received") not in broker.retained, "ir/received: QoS 0, not retained", str(pubs))
            sub.drain()   # wait for a hub object published after the off
            cmd.publish(BASE + "/cmd", json.dumps({"id": "i2", "action": "ir_capture", "enabled": False}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "i2")
            r = jload(m[1]) if m else {}
            check(r.get("ok") is True and r.get("enabled") is False, "cmd ir_capture off answered", str(r))
            mh = sub.wait_for(BASE + "/hub", 5, lambda p: (jload(p) or {}).get("ir_capture") is False)
            check(mh is not None and (retained_json(broker, BASE + "/hub") or {}).get("ir_capture") is False,
                  "hub object republished with ir_capture false", str(mh))
            os.remove(rx)
            cmd.close()
            sub.close()
            st, s = hub.http_get("/api/mqtt-status")
            check(s.get("v2") is True and s.get("connected") is True, "GET /api/mqtt-status: v2 and connected", str(s))
            st, s = hub.http_get("/api/status")
            check(s.get("mqtt", {}).get("v2") is True and "mqtt_v2" in s.get("capabilities", []), "status: mqtt.v2 and the capability")
            # settings save keeps the discovery fields and drops the per-remote ones
            sub2 = MockClient("127.0.0.1", broker.port, "observer2")
            ws_frames = []
            sub2.subscribe("homeassistant/#")
            sub2.subscribe(BASE + "/hub")
            hub_before = retained_json(broker, BASE + "/hub") or {}
            st, s = hub.http_post("/api/mqtt-save", {"baseTopic": BASE, "slot1RemoteEvents": False, "haDiscovery": False, "discoveryPrefix": "homeassistant"})
            with open(os.path.join(hub.data_dir, "mqtt.json")) as f:
                saved = json.load(f)
            check(st == 200 and "slot1RemoteEvents" not in saved and saved.get("haDiscovery") is False and saved.get("discoveryPrefix") == "homeassistant"
                  and saved.get("baseTopic") == BASE, "mqtt-save keeps haDiscovery and discoveryPrefix, drops the per-remote fields", str(saved))
            check(hub_before.get("ha_discovery") is True and hub_before.get("discovery_prefix") == "homeassistant",
                  "retained hub object carried ha_discovery true before the save", str({k: hub_before.get(k) for k in ("ha_discovery", "discovery_prefix")}))
            mh = sub2.wait_for(BASE + "/hub", 10, lambda p: (jload(p) or {}).get("ha_discovery") is False)
            check(mh is not None, "hub object republished with ha_discovery false after the save")
            deadline = time.time() + 10
            while time.time() < deadline and not any(f.get("connected") for f in ws_frames):
                try:
                    f = ws.recv_json(timeout=1)
                except Exception:
                    continue
                if f and f.get("type") == "mqtt_status":
                    ws_frames.append(f)
            check(len(ws_frames) >= 2 and ws_frames[0].get("connected") is False and ws_frames[-1].get("connected") is True,
                  "mqtt_status pushed on the save (disconnected) and on the reconnect (connected)", str(ws_frames))
            ws.sock.close()
            m = sub2.wait_for("homeassistant/select/harmony-test_activity/config", 10, lambda p: p == b"")
            check(m is not None and broker.retained.get("homeassistant/select/harmony-test_activity/config", b"") == b"",
                  "discovery off: the configs are cleared on reconnect", str(m))
            sub2.close()

        # "online" must mean commands are answered: a client sends a command the instant it sees the
        # Hub's live "online" (first connect, then two reconnects after a settings save). The Hub
        # subscribes before it publishes "online"; with the old order the broker dropped the command.
        print("[-] 5.3 a command sent the moment the Hub says online is answered")
        broker2 = MockBroker()
        try:
            cfg2 = dict(mqtt_cfg, port=broker2.port, haDiscovery=False)
            fast = MockClient("127.0.0.1", broker2.port, "fast-commander")
            sent = []

            def on_online(topic, payload, retain):
                if topic == BASE + "/status" and payload == b"online" and not retain:
                    cid = "online-%d" % (len(sent) + 1)
                    sent.append(cid)
                    fast.publish(BASE + "/cmd", json.dumps({"id": cid, "action": "get_state"}), qos=1)

            fast.on_message = on_online
            fast.subscribe(BASE + "/status")
            fast.subscribe(BASE + "/cmd/response")
            time.sleep(0.3)
            with HubHarness(port=PORT, args=["--mfg-file", mfg_path], env={"HARMONY_MQTT_LIVE": "1"},
                            extra_data_files={"mqtt.json": cfg2}) as hub2:
                for n in range(1, 4):
                    if n > 1:
                        hub2.http_post("/api/mqtt-save", {"baseTopic": BASE, "haDiscovery": False})
                    deadline = time.time() + 15
                    while time.time() < deadline and len(sent) < n:
                        time.sleep(0.05)
                    cid = "online-%d" % n
                    m = fast.wait_for(BASE + "/cmd/response", 5, lambda p, cid=cid: (jload(p) or {}).get("id") == cid)
                    check(len(sent) >= n and m is not None and (jload(m[1]) or {}).get("ok") is True,
                          "command sent right after \"online\" is answered (%s)" % ("first connect" if n == 1 else "reconnect %d" % (n - 1)),
                          "sent=%s reply=%s" % (sent, m))
                qos = [c.sub_qos.get(BASE + "/cmd") for c in list(broker2.clients) if BASE + "/cmd" in c.sub_qos]
                check(qos == [1], "the Hub subscribes to <base>/cmd at QoS 1", str(qos))
            fast.close()
        finally:
            broker2.close()
    finally:
        for p in (BT_STATUS_FILE, BT_STATUS_FILE + ".tmp", mfg_path):
            try:
                os.remove(p)
            except OSError:
                pass
        broker.close()
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all MQTT v2 checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
