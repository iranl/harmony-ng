#!/usr/bin/env python3
"""tools/migrate_hub_configs.py gives every device an id the RF protocol can carry.

The RF frames carry 4 bytes of a device id. A harmony-ng config or backup bundle can still
hold longer ids (an older stock import, devices added by other tools). The migration gives
those devices short ids, keeps the ids that already fit, and rewrites every reference:
activities (device list and steps), power sequences, the Bluetooth trigger, the remote maps
(slot 1, slot 2, Bluetooth remote; "deviceId" and "targetDevice" buttons, "devices" keys) and
the hosts of a legacy bt-devices.json.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import migrate_hub_configs as mig  # noqa: E402

LONG = "75563901"
NON_ASCII = "téle"  # 4 characters, 5 bytes in UTF-8
ADDR = "00:11:22:33:44:55"


def write_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj), encoding="utf-8")


def config():
    devices = {"devices": [
        {"id": "1", "name": "Amp", "type": "IR", "commands": [{"name": "PowerOn"}]},
        {"id": LONG, "name": "TV", "type": "IR", "commands": [{"name": "PowerOn"}, {"name": "InputHdmi1"}],
         "powerOnSequence": [{"deviceId": LONG, "command": "PowerOn"}, {"deviceId": "1", "command": "PowerOn"}]},
        {"id": "2", "name": "Player", "type": "BT", "transport": 32, "commands": [{"name": "Home"}],
         "btTriggerDeviceId": LONG, "btTriggerCommandName": "InputHdmi1"},
        {"id": NON_ASCII, "name": "Box", "type": "IR", "commands": [{"name": "PowerOn"}]},
    ]}
    activities = {"activities": [{"id": "1", "name": "Watch", "deviceIds": [LONG, "2", NON_ASCII],
                                  "startSequence": [{"deviceId": LONG, "command": "InputHdmi1"}],
                                  "stopSequence": [{"deviceId": NON_ASCII, "command": "PowerOn"}]}]}
    slot1 = {"activities": {"1": {"VolumeUp": {"type": "command", "action": "device_cmd",
                                                "deviceId": LONG, "command": "VolumeUp"},
                                  "Guide": {"targetDevice": NON_ASCII, "command": "Guide"}}},
             "devices": {LONG: {"OK": {"deviceId": LONG, "command": "OK"}},
                         "2": {"Home": {"deviceId": "2", "command": "Home"}}}}
    slot2 = {"activities": {"1": {"Play": {"targetDevice": LONG, "command": "Play"}}}}
    bt_map = {"activities": {"1": {"Up": {"deviceId": LONG, "command": "Up"}}}}
    bt_devices = {"devices": [{"id": "2", "bdaddr": ADDR}, {"id": LONG, "bdaddr": "00:11:22:33:44:66"}]}
    return devices, activities, slot1, slot2, bt_map, bt_devices


class MigrateDeviceIds(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, out: Path, bt_hosts: bool) -> None:
        devs = json.loads((out / "config" / "devices.json").read_text())["devices"]
        ids = {d["name"]: d["id"] for d in devs}
        # Ids that fit are kept; each long one gets the lowest free number
        self.assertEqual(ids, {"Amp": "1", "TV": "3", "Player": "2", "Box": "4"})
        by = {d["name"]: d for d in devs}
        self.assertEqual([s["deviceId"] for s in by["TV"]["powerOnSequence"]], ["3", "1"])
        self.assertEqual(by["Player"]["btTriggerDeviceId"], "3")
        if bt_hosts:
            # bt-devices.json names the ids of the same config: hosts land on the right device
            self.assertEqual(by["Player"]["bdaddr"], ADDR)
            self.assertEqual(by["TV"]["bdaddr"], "00:11:22:33:44:66")
        act = json.loads((out / "config" / "activities.json").read_text())["activities"][0]
        self.assertEqual(act["deviceIds"], ["3", "2", "4"])
        self.assertEqual(act["startSequence"][0]["deviceId"], "3")
        self.assertEqual(act["stopSequence"][0]["deviceId"], "4")
        slot1 = json.loads((out / "remote_slot1_map.json").read_text())
        self.assertEqual(slot1["activities"]["1"]["VolumeUp"]["deviceId"], "3")
        self.assertEqual(slot1["activities"]["1"]["Guide"]["targetDevice"], "4")
        self.assertEqual(sorted(slot1["devices"]), ["2", "3"])
        self.assertEqual(slot1["devices"]["3"]["OK"]["deviceId"], "3")
        slot2 = json.loads((out / "remote_slot2_map.json").read_text())
        self.assertEqual(slot2["activities"]["1"]["Play"]["targetDevice"], "3")
        bt_map = json.loads((out / "bt_remote_map.json").read_text())
        self.assertEqual(bt_map["activities"]["1"]["Up"]["deviceId"], "3")
        for d in devs:
            self.assertLessEqual(len(d["id"].encode("utf-8")), mig.RF_DEVICE_ID_MAX)

    def test_config_folder(self):
        devices, activities, slot1, slot2, bt_map, bt_devices = config()
        cfg = self.base / "harmony"
        write_json(cfg / "config" / "devices.json", devices)
        write_json(cfg / "config" / "activities.json", activities)
        write_json(cfg / "remote_slot1_map.json", slot1)
        write_json(cfg / "remote_slot2_map.json", slot2)
        write_json(cfg / "bt_remote_map.json", bt_map)
        write_json(cfg / "bt-devices.json", bt_devices)
        out = self.base / "out"
        stats = mig.migrate(str(cfg), str(out))
        self.assertTrue(any("renumbered" in w for w in stats["warnings"]), stats["warnings"])
        self.check(out, bt_hosts=True)

    def test_bundle(self):
        devices, activities, slot1, slot2, bt_map, bt_devices = config()
        bundle = self.base / "bundle.json"
        write_json(bundle, {"devices": devices, "activities": activities, "remote_slot1_map": slot1,
                            "remote_slot2_map": slot2, "bt_remote_map": bt_map, "bluetooth": bt_devices})
        out = self.base / "out"
        mig.migrate(str(bundle), str(out))
        self.check(out, bt_hosts=True)

    def stale_config(self):
        # Device 2 was deleted, but a step and a map entry still name it
        devices = {"devices": [{"id": "1", "name": "Amp", "type": "IR", "commands": [{"name": "PowerOn"}]},
                               {"id": LONG, "name": "TV", "type": "IR", "commands": [{"name": "PowerOn"}]}]}
        activities = {"activities": [{"id": "1", "name": "Watch", "deviceIds": ["1", LONG],
                                      "stopSequence": [{"deviceId": "2", "command": "PowerOff"}]}]}
        slot1 = {"activities": {"1": {"Play": {"deviceId": "3", "command": "Play"}}},
                 "devices": {"2": {"OK": {"deviceId": "2", "command": "OK"}},
                             LONG: {"OK": {"deviceId": LONG, "command": "OK"}}}}
        return devices, activities, slot1

    def check_stale(self, out: Path) -> None:
        devs = json.loads((out / "config" / "devices.json").read_text())["devices"]
        tv = next(d["id"] for d in devs if d["name"] == "TV")
        # 2 is referenced by the stale step and map entry, 3 by a stale map button: both stay free
        self.assertEqual(tv, "4")
        act = json.loads((out / "config" / "activities.json").read_text())["activities"][0]
        self.assertEqual(act["stopSequence"][0]["deviceId"], "2")
        slot1 = json.loads((out / "remote_slot1_map.json").read_text())
        self.assertEqual(sorted(slot1["devices"]), ["2", "4"])
        self.assertEqual(slot1["devices"]["4"]["OK"]["deviceId"], "4")
        self.assertEqual(slot1["devices"]["2"]["OK"]["deviceId"], "2")

    def test_stale_references_keep_their_id_folder(self):
        devices, activities, slot1 = self.stale_config()
        cfg = self.base / "harmony"
        write_json(cfg / "config" / "devices.json", devices)
        write_json(cfg / "config" / "activities.json", activities)
        write_json(cfg / "remote_slot1_map.json", slot1)
        mig.migrate(str(cfg), str(self.base / "out"))
        self.check_stale(self.base / "out")

    def test_stale_references_keep_their_id_bundle(self):
        devices, activities, slot1 = self.stale_config()
        bundle = self.base / "bundle.json"
        write_json(bundle, {"devices": devices, "activities": activities, "remote_slot1_map": slot1})
        mig.migrate(str(bundle), str(self.base / "out"))
        self.check_stale(self.base / "out")

    def test_ids_that_fit_are_not_touched(self):
        devices = {"devices": [{"id": "12", "name": "A"}, {"id": "abcd", "name": "B"}]}
        acts = {"activities": [{"id": "1", "name": "Watch", "deviceIds": ["12", "abcd"]}]}
        self.assertEqual(mig.shorten_long_device_ids(devices, acts, []), {})
        self.assertEqual([d["id"] for d in devices["devices"]], ["12", "abcd"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
