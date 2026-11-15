#!/usr/bin/env python3
"""
Deleting a device that is still used is refused (as agreed in #220): POST /api/device-delete
(and /ir/delete-device) answers 409 with the list of users and changes nothing.
- users are activities (start/stop sequences, deviceIds; with id, name and the fields), other
  devices whose Bluetooth trigger names it, and key bindings of the touchscreen remote maps
  (slot) and of the Bluetooth remote map ("activities/10/VolumeUp")
- the map's own page for the device ("devices/<id>") does not count
- a device nothing uses is deleted as before (200, ok true); an unknown device is not a 409
The daemon runs with --dry-run on a private mock root (port 9202).
"""

import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 9202
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
    m["activities"]["11"]["screen_buttons"] = [{"label": "Old", "deviceId": "4", "command": "Menu"}]
    json.dump(m, open(os.path.join(d, "remote_slot1_map.json"), "w"))
    shutil.copy(os.path.join(SAMPLE_CONFIG, "settings.json"), os.path.join(d, "settings.json"))
    return d


def device_ids(hub):
    st, body = hub.http_get("/api/devices")
    return [str(x.get("id")) for x in (body.get("devices", []) if isinstance(body, dict) else [])]


def users_of(r, kind):
    return [u for u in (r.get("users") or []) if u.get("type") == kind] if isinstance(r, dict) else []


def main():
    cfg = config_dir()
    ble_map = {"activities": {"10": {"OK": {"action": "device_cmd", "deviceId": "7", "command": "Select"}}}}
    try:
        with HubHarness(port=PORT, config_dir=cfg, extra_data_files={"bt_remote_map.json": ble_map}) as hub:
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
            check("1" in device_ids(hub), "device 1 still there")

            print("[-] a device only in an activity's device list")
            st, r = hub.http_post("/ir/delete-device?id=3", {"deviceId": "3", "id": "3"})
            check(st == 409 and [u.get("id") for u in users_of(r, "activity")] == ["12"]
                  and users_of(r, "activity")[0].get("fields") == ["deviceIds"], "deviceIds counts (legacy route)",
                  "%s %s" % (st, r))

            print("[-] a device named by another device's Bluetooth trigger and a screen button")
            st, r = hub.http_post("/api/device-delete", {"deviceId": "4"})
            devs = users_of(r, "device")
            maps = users_of(r, "remote_map")
            check(st == 409 and len(devs) == 1 and devs[0].get("id") == "5" and devs[0].get("field") == "btTriggerDeviceId",
                  "Bluetooth trigger of device 5 listed", "%s %s" % (st, r))
            check(len(maps) == 1 and "activities/11/screen_buttons/0" in maps[0].get("keys", []),
                  "screen button listed", str(maps))

            print("[-] a device bound on the Bluetooth remote")
            st, r = hub.http_post("/api/device-delete", {"deviceId": "7"})
            ble = users_of(r, "bt_remote_map")
            check(st == 409 and len(ble) == 1 and ble[0].get("keys") == ["activities/10/OK"] and "slot" not in ble[0],
                  "Bluetooth remote map listed", "%s %s" % (st, r))

            print("[-] devices nothing uses")
            st, r = hub.http_post("/api/device-delete", {"deviceId": "6"})
            check(st == 200 and isinstance(r, dict) and r.get("ok") is True, "only its own map page: deleted", "%s %s" % (st, r))
            check("6" not in device_ids(hub), "device 6 gone")
            st, r = hub.http_post("/api/device-delete", {"deviceId": "99"})
            check(st == 200 and isinstance(r, dict) and r.get("ok") is False, "unknown device: not found as before",
                  "%s %s" % (st, r))
            st, r = hub.http_post("/api/device-delete", {"deviceId": "5"})
            check(st == 200 and r.get("ok") is True, "device with a trigger of its own: deleted", "%s %s" % (st, r))
            st, r = hub.http_post("/api/device-delete", {"deviceId": "4"})
            maps = users_of(r, "remote_map")
            check(st == 409 and not users_of(r, "device") and len(maps) == 1,
                  "after the trigger went: only the screen button is left", "%s %s" % (st, r))
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("FAILED: %d check(s)" % len(failures))
        sys.exit(1)
    print("All device delete checks passed")


if __name__ == "__main__":
    main()
