#!/usr/bin/env python3
"""
Deleting a device that is still used is refused (as agreed in #220): POST /api/device-delete
(and /ir/delete-device) answers 409 with the list of users and changes nothing.
- users are activities (start/stop sequences, deviceIds; with id, name and the fields), other
  devices whose Bluetooth trigger names it, and key bindings of the touchscreen remote maps
  (slot) and of the Bluetooth remote map ("activities/10/VolumeUp")
- the map's own page for the device ("devices/<id>") does not count
- a device nothing uses is deleted as before (200, ok true); an unknown device is not a 409
- a device never becomes undeletable: bindings in a map section whose activity no longer exists
  and in the map of a slot with no paired remote (from hub_rf's status; without an answer, a slot
  with a version file) do not count; deleting an activity removes its sections from the maps; a
  deleted device's own page is removed from the maps, so a new device with the same id starts clean
- pages of devices that no longer exist do not count, and go with the next device delete
- a slot counts when hub_rf reports it active, or, inactive, while its version file names a remote
  (remote_id or skin: an unpair clears them); hub_rf down: when its version file exists; a hub_rf that answers slowly does not hold the request past ~1.5 s
- a map rewritten by a delete stays compact: a large map stays readable by the editors
The daemon runs with --dry-run on a private mock root (port 9202); a fake hub_rf status on 9203.
"""

import http.server
import json
import os
import shutil
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 9202
RF_PORT = 9203
failures = []


def check(cond, name, detail=""):
    print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
    if not cond:
        failures.append(name)


def config_dir():
    d = tempfile.mkdtemp(prefix="dev_delete_")
    devs = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devs["devices"].append({"id": "3", "name": "Spare Player", "type": "MediaPlayer", "commands": []})
    devs["devices"].append({"id": "4", "name": "Old Box", "type": "MediaPlayer", "commands": []})
    devs["devices"].append({"id": "5", "name": "Receiver", "type": "BT", "commands": [],
                            "btTriggerDeviceId": "4", "btTriggerCommandName": "Wake"})
    devs["devices"].append({"id": "6", "name": "Only Its Own Page", "type": "MediaPlayer", "commands": []})
    devs["devices"].append({"id": "7", "name": "Ble Target", "type": "MediaPlayer", "commands": []})
    json.dump(devs, open(os.path.join(d, "devices.json"), "w"))
    acts = json.load(open(os.path.join(SAMPLE_CONFIG, "activities.json")))
    acts["activities"].append({"id": "12", "name": "Spare", "type": "Custom", "order": 3, "deviceIds": ["3"],
                               "startSequence": [{"type": "Delay", "delay": 100}], "stopSequence": []})
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    m = json.load(open(os.path.join(SAMPLE_CONFIG, "remote_slot1_map.json")))
    m["devices"]["6"] = {"Play": {"type": "command", "action": "device_cmd", "deviceId": "6", "command": "Play"}}
    m["devices"]["3"] = {"Play": {"type": "command", "action": "device_cmd", "deviceId": "3", "command": "Play"}}
    # device 7's page binds a key to device 4
    m["devices"]["7"] = {"Z": {"type": "command", "action": "device_cmd", "deviceId": "4", "command": "Menu"}}
    m["activities"]["11"]["screen_buttons"] = [{"label": "Old", "deviceId": "4", "command": "Menu"}]
    m["activities"]["12"] = {"A": {"type": "command", "action": "device_cmd", "deviceId": "3", "command": "Play"}}
    m["activities"]["99"] = {"X": {"type": "command", "action": "device_cmd", "deviceId": "4", "command": "Menu"}}
    # the page of a device deleted before pages went with their device
    m["devices"]["98"] = {"Y": {"type": "command", "action": "device_cmd", "deviceId": "4", "command": "Menu"}}
    json.dump(m, open(os.path.join(d, "remote_slot1_map.json"), "w"))
    # an old slot 2 map: no remote paired there
    json.dump({"activities": {"10": {"B": {"type": "command", "action": "device_cmd", "deviceId": "4", "command": "Menu"}}},
               "devices": {}}, open(os.path.join(d, "remote_slot2_map.json"), "w"))
    shutil.copy(os.path.join(SAMPLE_CONFIG, "settings.json"), os.path.join(d, "settings.json"))
    return d


def device_ids(hub):
    st, body = hub.http_get("/api/devices")
    return [str(x.get("id")) for x in (body.get("devices", []) if isinstance(body, dict) else [])]


def users_of(r, kind):
    return [u for u in (r.get("users") or []) if u.get("type") == kind] if isinstance(r, dict) else []


class FakeRf(http.server.BaseHTTPRequestHandler):
    slot1_active = True
    trickle = False

    def do_GET(self):
        body = json.dumps({"ok": True, "slots": [{"slot": 1, "active": FakeRf.slot1_active},
                                                 {"slot": 2, "active": False}]}).encode()
        if FakeRf.trickle:  # one byte every 0.3 s: 20 s for the whole answer
            for ch in b"HTTP/1.1 200 OK\r\n\r\n" + body:
                try:
                    self.wfile.write(bytes([ch]))
                    self.wfile.flush()
                except OSError:
                    return
                time.sleep(0.3)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def map_file(hub, slot):
    with open(os.path.join(hub.data_dir, "remote_slot%d_map.json" % slot)) as f:
        return json.load(f)


def main():
    cfg = config_dir()
    ble_map = {"activities": {"10": {"OK": {"action": "device_cmd", "deviceId": "7", "command": "Select"}}}}
    rf = http.server.ThreadingHTTPServer(("127.0.0.1", RF_PORT), FakeRf)
    rf.daemon_threads = True
    threading.Thread(target=rf.serve_forever, daemon=True).start()
    try:
        with HubHarness(port=PORT, config_dir=cfg, env={"HARMONY_TEST_RF_PORT": str(RF_PORT)},
                        extra_data_files={"bt_remote_map.json": ble_map}) as hub:
            print("[-] a device in activity sequences and remote keys")
            st, r = hub.http_post("/api/device-delete", {"deviceId": "1"})
            check(st == 409 and isinstance(r, dict) and r.get("ok") is False and "error" in r, "delete refused: 409",
                  "%s %s" % (st, r))
            acts = {u.get("id"): u for u in users_of(r, "activity")}
            check(set(acts) == {"10", "11"} and acts["10"].get("name") == "Watch TV"
                  and "startSequence" in acts["10"].get("fields", []) and "stopSequence" in acts["10"].get("fields", []),
                  "activities 10 and 11 listed with name and fields", str(acts))
            maps = users_of(r, "remote_map")
            keys = maps[0].get("keys", []) if maps else []
            check(len(maps) == 1 and maps[0].get("slot") == 1 and "activities/10/VolumeUp" in keys
                  and "activities/11/VolumeDown" in keys and not any(k.startswith("devices/1") for k in keys),
                  "slot 1 key bindings listed, not the device's own page", str(maps))
            groups = sorted((g.get("type"), g.get("id"), g.get("name"), g.get("count")) for g in (maps[0].get("groups", []) if maps else []))
            check(groups == [("activity", "10", "Watch TV", 2), ("activity", "11", "Media Player", 2)],
                  "keys counted per activity, with its name", str(maps))
            check("1" in device_ids(hub), "device 1 still there")

            print("[-] a device only in an activity's device list")
            st, r = hub.http_post("/ir/delete-device?id=3", {"deviceId": "3", "id": "3"})
            check(st == 409 and [u.get("id") for u in users_of(r, "activity")] == ["12"]
                  and users_of(r, "activity")[0].get("fields") == ["deviceIds"], "deviceIds counts (legacy route)",
                  "%s %s" % (st, r))
            check([u.get("keys") for u in users_of(r, "remote_map")] == [["activities/12/A"]],
                  "and the activity's key in slot 1", str(r))

            print("[-] deleting the activity removes its map sections; then the device goes")
            st, r = hub.http_post("/api/activity-delete", {"id": "12"})
            check(st == 200 and r.get("ok") is True, "activity 12 deleted", "%s %s" % (st, r))
            check("12" not in map_file(hub, 1).get("activities", {}), "its section left the slot 1 map",
                  str(map_file(hub, 1).get("activities", {}).keys()))
            st, r = hub.http_post("/api/device-delete", {"deviceId": "3"})
            check(st == 200 and r.get("ok") is True, "device 3 deleted", "%s %s" % (st, r))

            print("[-] a device named by another device's Bluetooth trigger and a screen button")
            st, r = hub.http_post("/api/device-delete", {"deviceId": "4"})
            devs = users_of(r, "device")
            maps = users_of(r, "remote_map")
            check(st == 409 and len(devs) == 1 and devs[0].get("id") == "5" and devs[0].get("field") == "btTriggerDeviceId",
                  "Bluetooth trigger of device 5 listed", "%s %s" % (st, r))
            check(len(maps) == 1 and sorted(maps[0].get("keys", [])) == ["activities/11/screen_buttons/0", "devices/7/Z"],
                  "screen button and another device's page listed; not a deleted activity's section, not the unpaired slot 2",
                  str(maps))
            groups = sorted((maps[0].get("groups") if maps else []) or [], key=lambda g: g.get("id"))
            check(groups == [{"type": "activity", "id": "11", "name": "Media Player", "count": 1},
                             {"type": "device_page", "id": "7", "name": "Ble Target", "count": 1}],
                  "a screen button counts as a key of its activity; a device page is named", str(maps))

            print("[-] a device bound on the Bluetooth remote")
            st, r = hub.http_post("/api/device-delete", {"deviceId": "7"})
            ble = users_of(r, "bt_remote_map")
            check(st == 409 and len(ble) == 1 and ble[0].get("keys") == ["activities/10/OK"] and "slot" not in ble[0],
                  "Bluetooth remote map listed", "%s %s" % (st, r))

            print("[-] devices nothing uses")
            st, r = hub.http_post("/api/device-delete", {"deviceId": "6"})
            check(st == 200 and isinstance(r, dict) and r.get("ok") is True, "only its own map page: deleted", "%s %s" % (st, r))
            check("6" not in device_ids(hub), "device 6 gone")
            check("6" not in map_file(hub, 1).get("devices", {}), "its own page left the map")
            check("98" not in map_file(hub, 1).get("devices", {}), "the leftover page of a deleted device went too")
            st, r = hub.http_post("/api/device-save", {"name": "New Player", "type": "MediaPlayer"})
            new_id = str(r.get("deviceId", r.get("id")))
            check(st == 200 and new_id in ("3", "6") and new_id not in map_file(hub, 1).get("devices", {}),
                  "a new device given a freed id has no inherited page", "%s %s" % (st, r))
            st, r = hub.http_post("/api/device-delete", {"deviceId": "99"})
            check(st == 200 and isinstance(r, dict) and r.get("ok") is False, "unknown device: not found as before",
                  "%s %s" % (st, r))
            st, r = hub.http_post("/api/device-delete", {"deviceId": "5"})
            check(st == 200 and r.get("ok") is True, "device with a trigger of its own: deleted", "%s %s" % (st, r))
            st, r = hub.http_post("/api/device-delete", {"deviceId": "4"})
            maps = users_of(r, "remote_map")
            check(st == 409 and not users_of(r, "device") and len(maps) == 1,
                  "after the trigger went: only the screen button is left", "%s %s" % (st, r))

        print("[-] hub_rf says the slot is not active: counted only while its version file names a remote")
        FakeRf.slot1_active = False
        with HubHarness(port=PORT, config_dir=cfg, env={"HARMONY_TEST_RF_PORT": str(RF_PORT)},
                        extra_data_files={"remote_slot1_version.json": {"slot": 1, "version": "2.4.2",
                                                                         "remote_id": "20141001120000-000002"}}) as hub:
            st, r = hub.http_post("/api/device-delete", {"deviceId": "4"})
            maps = {u.get("slot"): u.get("keys") for u in users_of(r, "remote_map")}
            check(st == 409 and 1 in maps and "activities/11/screen_buttons/0" in maps[1], "slot 1 counted: its file names a remote",
                  "%s %s" % (st, r))
        with HubHarness(port=PORT, config_dir=cfg, env={"HARMONY_TEST_RF_PORT": str(RF_PORT)},
                        extra_data_files={"remote_slot1_version.json": {"slot": 1, "version": "2.4.2"}}) as hub:
            st, r = hub.http_post("/api/device-delete", {"deviceId": "4"})
            check(not users_of(r, "remote_map"), "slot 1 unpaired (no remote_id or skin in its file): not counted",
                  "%s %s" % (st, r))
        FakeRf.slot1_active = True

        print("[-] a slow hub_rf does not hold the request")
        FakeRf.trickle = True
        with HubHarness(port=PORT, config_dir=cfg, env={"HARMONY_TEST_RF_PORT": str(RF_PORT)}) as hub:
            t0 = time.time()
            st, r = hub.http_post("/api/device-delete", {"deviceId": "4"}, timeout=10)
            took = time.time() - t0
            check(st == 409 and took < 2.5, "answered in %.1f s" % took, "%s %s" % (st, r))
        FakeRf.trickle = False

        print("[-] a large compact map stays readable after a delete rewrote it")
        big = {"activities": {"10": {}, "12": {"A": {"type": "command", "action": "device_cmd", "deviceId": "3", "command": "x"}}},
               "devices": {}}
        for i in range(1750):
            big["activities"]["10"]["K%04d" % i] = {"type": "command", "action": "device_cmd", "deviceId": "1",
                                                     "command": "Command%04d" % i}
        compact = json.dumps(big, separators=(",", ":"))
        with HubHarness(port=PORT, config_dir=cfg, env={"HARMONY_TEST_RF_PORT": str(RF_PORT)},
                        extra_data_files={"remote_slot1_map.json": compact}) as hub:
            st, r = hub.http_post("/api/activity-delete", {"id": "12"})
            size = os.path.getsize(os.path.join(hub.data_dir, "remote_slot1_map.json"))
            st2, m2 = hub.http_get("/api/rf-mapping?slot=1")
            check(st == 200 and size <= len(compact) and size < 200000 and "12" not in map_file(hub, 1)["activities"]
                  and len((m2 or {}).get("activities", {}).get("10", {})) == 1750,
                  "map written compact (%d bytes, was %d) and still read by GET /api/rf-mapping" % (size, len(compact)),
                  "%s %s" % (st2, str(m2)[:200]))

        print("[-] two activity deletes at once both leave the map; a temp file left by a power cut goes")
        acts = json.load(open(os.path.join(cfg, "activities.json")))
        acts["activities"] += [{"id": str(i), "name": "A%d" % i, "type": "Custom", "order": i,
                                "startSequence": [], "stopSequence": []} for i in (20, 21, 22, 23)]
        two = os.path.join(cfg, "two")
        os.makedirs(two, exist_ok=True)
        for name in ("devices.json", "settings.json"):
            shutil.copy(os.path.join(cfg, name), os.path.join(two, name))
        json.dump(acts, open(os.path.join(two, "activities.json"), "w"))
        m = {"activities": {str(i): {"K%d" % k: {"type": "command", "action": "device_cmd", "deviceId": "1",
                                                 "command": "C%d" % k} for k in range(300)} for i in (10, 20, 21, 22, 23)},
             "devices": {}}
        json.dump(m, open(os.path.join(two, "remote_slot1_map.json"), "w"), separators=(",", ":"))
        with HubHarness(port=PORT, config_dir=two, env={"HARMONY_TEST_RF_PORT": str(RF_PORT)},
                        extra_data_files={"remote_slot1_map.json.drop": "half a map from a power cut"}) as hub:
            results = []
            ths = [threading.Thread(target=lambda a=a: results.append(hub.http_post("/api/activity-delete", {"id": a})[0]))
                   for a in ("20", "21", "22", "23")]
            for th in ths:
                th.start()
            for th in ths:
                th.join()
            left = sorted(map_file(hub, 1)["activities"])
            check(results == [200] * 4 and left == ["10"], "all four sections left the map", "%s %s" % (results, left))
            leftovers = [n for n in os.listdir(hub.data_dir) if ".drop" in n]
            check(leftovers == [], "no temporary map file left", str(leftovers))

        print("[-] without hub_rf: a slot counts when its version file exists")
        rf.shutdown()
        with HubHarness(port=PORT, config_dir=cfg, env={"HARMONY_TEST_RF_PORT": str(RF_PORT)},
                        extra_data_files={"remote_slot2_version.json": {"slot": 2, "version": "2.4.2"}}) as hub:
            st, r = hub.http_post("/api/device-delete", {"deviceId": "4"})
            maps = {u.get("slot"): u.get("keys") for u in users_of(r, "remote_map")}
            check(st == 409 and maps == {2: ["activities/10/B"]}, "slot 2 counted, slot 1 without a version file not",
                  "%s %s" % (st, r))
    finally:
        rf.server_close()
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("FAILED: %d check(s)" % len(failures))
        sys.exit(1)
    print("All device delete checks passed")


if __name__ == "__main__":
    main()
