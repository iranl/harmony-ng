#!/usr/bin/env python3
"""tools/migrate_hub_configs.py on input it says it accepts, and on secrets it writes.

- commands given as plain names, activity ids such as "tv", non-object list entries and
  numbers written as text must not stop the whole migration;
- a stock DeviceList.json without ActivityList.json migrates the devices and warns;
- a failed migration leaves nothing in --dest and says why on stderr;
- a bundle with unreadable Bluetooth link keys says so instead of reporting success;
- the Wi-Fi passphrase, MQTT password and link keys are written readable by the owner only;
- a codex folder or bundle that still holds 8-digit stock device ids gets short ids, in the
  activities and in the remote maps (the RF side cannot match the long ones).
"""

from __future__ import annotations

import base64
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TOOL = REPO_ROOT / "tools" / "migrate_hub_configs.py"
sys.path.insert(0, str(REPO_ROOT / "tools"))

import migrate_hub_configs as mig  # noqa: E402


def write_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj))


def mode(path: Path) -> int:
    return stat.S_IMODE(path.stat().st_mode)


class MigrateInputShapes(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.old_umask = os.umask(0o022)

    def tearDown(self):
        os.umask(self.old_umask)
        self.tmp.cleanup()

    def test_string_commands(self):
        out = mig.sanitize_devices_json({"devices": [{"id": "1", "name": "TV", "commands": ["PowerOn", 7, {"name": "Mute"}]}]})
        names = [c["name"] for c in out["devices"][0]["commands"]]
        self.assertEqual(names, ["PowerOn", "Mute"])

    def test_short_text_activity_id_and_non_object_entry(self):
        acts, id_map = mig.sanitize_activities_json({"activities": ["junk", {"id": "tv", "name": "TV"},
                                                                    {"id": "2", "name": "Movie", "order": "first"}]})
        self.assertEqual([a["name"] for a in acts["activities"]], ["TV", "Movie"])
        self.assertEqual(id_map, {"tv": "1", "2": "2"})
        self.assertEqual(acts["activities"][1]["order"], 0)

    def test_numbers_written_as_text(self):
        out = mig.sanitize_devices_json({"devices": [{"id": "1", "name": "TV", "powerOnDelay": "2500",
                                                      "interKeyDelay": "fast", "btAutoReconnect": 0,
                                                      "commands": [{"name": "Up", "repeats": "2", "port": "x"}]}]})
        d = out["devices"][0]
        self.assertEqual((d["powerOnDelay"], d["interKeyDelay"], d["btAutoReconnect"]), (2500, 100, 0))
        self.assertEqual((d["commands"][0]["repeats"], d["commands"][0]["port"]), (2, 7))

    def test_stock_activity_order_as_text(self):
        res = self.base / "resources"
        write_json(res / "DeviceList.json", {"DevicesWithFeatures": [
            {"Device": {"Id-": "75563901", "Name": "TV", "Transport": "1", "InterKeyDelay": "x"},
             "Commands": [{"Name": "PowerOn", "KeyCode": "G:NEC:()(0x20DF10EF)():3"}]}]})
        write_json(res / "ActivityList.json", {"Activities": [
            {"Id-": "33333333", "Name": "Watch", "ActivityOrder": "first",
             "Roles": [{"DeviceId-": "75563901", "PowerOnOrder": "a"}]}]})
        stats = mig.migrate(str(res), str(self.base / "out"))
        self.assertEqual((stats["devices"], stats["activities"]), (1, 1))

    def test_stock_without_activity_list(self):
        res = self.base / "resources"
        write_json(res / "DeviceList.json", {"DevicesWithFeatures": [
            {"Device": {"Id-": "75563901", "Name": "TV"}, "Commands": [{"Name": "PowerOn", "KeyCode": "x"}]}]})
        stats = mig.migrate(str(res), str(self.base / "out"))
        self.assertEqual(stats["devices"], 1)
        self.assertTrue(any("ActivityList.json" in w for w in stats["warnings"]), stats["warnings"])

    def test_failure_leaves_dest_empty_and_reports_on_stderr(self):
        codex = self.base / "harmony"
        # Valid devices, then a remote map that is not JSON: the run fails after devices.json
        # was written, and must not leave it behind
        write_json(codex / "config" / "devices.json", {"devices": [{"id": "1", "name": "TV"}]})
        (codex / "remote_slot1_map.json").write_text("{cut")
        dest = self.base / "out"
        dest.mkdir()
        res = subprocess.run([sys.executable, str(TOOL), "--src", str(codex), "--dest", str(dest)],
                             capture_output=True, text=True, timeout=60)
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertIn("Migration failed", res.stderr)
        self.assertEqual(sorted(p.name for p in dest.iterdir()), [], "a failed run left files in --dest")

    def test_bundle_bad_link_keys_warns(self):
        bundle = self.base / "bundle.json"
        write_json(bundle, {"devices": {"devices": [{"id": "1", "name": "TV"}]}, "btstack_keys": "not*base64"})
        stats = mig.migrate(str(bundle), str(self.base / "out"))
        self.assertFalse(stats["bt_keys"])
        self.assertTrue(any("link keys" in w for w in stats["warnings"]), stats["warnings"])

    def test_secrets_owner_only(self):
        bundle = self.base / "bundle.json"
        write_json(bundle, {"devices": {"devices": [{"id": "1", "name": "TV"}]},
                            "mqtt": {"password": "placeholder"}, "wifi": "network={psk=\"placeholder\"}",
                            "btstack_keys": base64.b64encode(b"\x01\x02\x03").decode()})
        out = self.base / "out"
        stats = mig.migrate(str(bundle), str(out))
        self.assertTrue(stats["bt_keys"])
        for name in ("mqtt.json", "wpa_supplicant.conf", "btstack_keys.tlv", "btstack_keys.bin"):
            self.assertEqual(mode(out / name), 0o600, name)
        self.assertEqual(mode(out), 0o700)
        self.assertEqual((out / "btstack_keys.tlv").read_bytes(), b"\x01\x02\x03")

        codex = self.base / "codex"
        write_json(codex / "config" / "devices.json", {"devices": [{"id": "1", "name": "TV"}]})
        write_json(codex / "mqtt.json", {"password": "placeholder"})
        (codex / "btstack_keys.tlv").write_bytes(b"\x01")
        out2 = self.base / "out2"
        mig.migrate(str(codex), str(out2))
        self.assertEqual(mode(out2 / "mqtt.json"), 0o600)
        self.assertEqual(mode(out2 / "btstack_keys.tlv"), 0o600)

    def long_id_config(self):
        devices = {"devices": [{"id": "75563901", "name": "TV", "commands": [{"name": "PowerOn"}]},
                               {"id": "2", "name": "Player", "commands": [{"name": "Play"}]}]}
        activities = {"activities": [{"id": "1", "name": "Watch", "deviceIds": ["75563901", "2"],
                                      "startSequence": [{"deviceId": "75563901", "command": "PowerOn"}]}]}
        slot1 = {"activities": {"1": {"VolumeUp": {"type": "command", "action": "device_cmd",
                                                    "deviceId": "75563901", "command": "VolumeUp"}}},
                 "devices": {"75563901": {"Select": {"deviceId": "75563901", "command": "OK"}},
                             "2": {"Play": {"deviceId": "2", "command": "Play"}}}}
        return devices, activities, slot1

    def check_short_ids(self, out: Path) -> None:
        devs = json.loads((out / "config" / "devices.json").read_text())["devices"]
        ids = {d["name"]: d["id"] for d in devs}
        self.assertTrue(all(len(i) <= mig.RF_DEVICE_ID_MAX for i in ids.values()), ids)
        acts = json.loads((out / "config" / "activities.json").read_text())["activities"]
        self.assertEqual(acts[0]["deviceIds"], [ids["TV"], ids["Player"]])
        self.assertEqual(acts[0]["startSequence"][0]["deviceId"], ids["TV"])
        slot1 = json.loads((out / "remote_slot1_map.json").read_text())
        self.assertEqual(slot1["activities"]["1"]["VolumeUp"]["deviceId"], ids["TV"])
        self.assertEqual(sorted(slot1["devices"]), sorted([ids["TV"], ids["Player"]]))
        self.assertEqual(slot1["devices"][ids["TV"]]["Select"]["deviceId"], ids["TV"])
        self.assertEqual(slot1["devices"][ids["Player"]]["Play"]["deviceId"], ids["Player"])

    def test_long_ids_codex_folder(self):
        devices, activities, slot1 = self.long_id_config()
        codex = self.base / "harmony"
        write_json(codex / "config" / "devices.json", devices)
        write_json(codex / "config" / "activities.json", activities)
        write_json(codex / "remote_slot1_map.json", slot1)
        out = self.base / "out"
        mig.migrate(str(codex), str(out))
        self.check_short_ids(out)

    def test_long_ids_bundle(self):
        devices, activities, slot1 = self.long_id_config()
        bundle = self.base / "bundle.json"
        write_json(bundle, {"devices": devices, "activities": activities, "remote_slot1_map": slot1})
        out = self.base / "out"
        stats = mig.migrate(str(bundle), str(out))
        self.assertTrue(any("renumbered" in w for w in stats["warnings"]), stats["warnings"])
        self.check_short_ids(out)

    def test_short_ids_unchanged(self):
        devices = {"devices": [{"id": "1", "name": "TV"}, {"id": "12", "name": "Player"}]}
        acts = {"activities": [{"id": "1", "name": "Watch", "deviceIds": ["12"]}]}
        self.assertEqual(mig.shorten_long_device_ids(devices, acts, []), {})
        self.assertEqual([d["id"] for d in devices["devices"]], ["1", "12"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
