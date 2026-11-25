#!/usr/bin/env python3
"""
test_migrate_companion_maps.py - Stock maps of two remotes on one Hub (tools/migrate_hub_configs.py)

Every remote of a Hub carries the Hub's RemoteId in MapList.json, so the remote model in the
ButtonMapIdentifier (16417 a touchscreen remote, 16422 a Companion) is what keeps their maps
apart. A Companion's activity keys are in its Root map, as ButtonActivityActions.
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import migrate_hub_configs as mig  # noqa: E402
from test_migrate_stock_config import ACTIVITY_LIST, DEVICE_LIST, LISTEN, SOUNDBAR, TV, WATCH, hard, soft  # noqa: E402

REMOTE_ID = 1234567


def starts(key, activity, long_activity=None):
    b = {"__type": "HardRemoteButton", "ButtonKey": key,
         "ButtonAction": {"__type": "ButtonActivityAction", "ActivityId-": activity}}
    if long_activity:
        b["ButtonLongPressAction"] = {"__type": "ButtonActivityAction", "ActivityId-": long_activity}
    return b


MAP_LIST = {"ButtonMaps": [
    {"ButtonMapIdentifier": f"16417Activity{WATCH}", "RemoteId-": REMOTE_ID, "Buttons": [
        hard("Guide", TV, "SmartHub"),
        soft(0, TV, "Exit"),
    ]},
    # A touchscreen remote's Root map with a device command: not imported, as hub_rf would send
    # it in every activity that leaves the key unmapped
    {"ButtonMapIdentifier": "16417Root", "RemoteId-": REMOTE_ID, "Buttons": [
        hard("VolumeUp", SOUNDBAR, "VolumeUp"),
    ]},
    {"ButtonMapIdentifier": f"16422Activity{WATCH}", "RemoteId-": REMOTE_ID, "Buttons": [
        hard("Guide", TV, "Select"),
        hard("PrevChannel", TV, "Return"),
        hard("VolumeMute", SOUNDBAR, "Mute"),
    ]},
    # A Companion map with no buttons: the activity gets the default keys
    {"ButtonMapIdentifier": f"16422Activity{LISTEN}", "RemoteId-": REMOTE_ID, "Buttons": []},
    {"ButtonMapIdentifier": "16422Root", "RemoteId-": REMOTE_ID, "Buttons": [
        starts("WatchTVActivity", WATCH, long_activity=LISTEN),
        starts("MovieActivity", LISTEN),
        starts("MusicActivity", 99999999),
    ]},
]}


class CompanionMapsTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        res = root / "src" / "data" / "resources"
        res.mkdir(parents=True)
        (res / "DeviceList.json").write_text(json.dumps(DEVICE_LIST))
        (res / "ActivityList.json").write_text(json.dumps(ACTIVITY_LIST))
        (res / "MapList.json").write_text(json.dumps(MAP_LIST))
        dest = root / "dest"
        cls.stats = mig.migrate(str(root / "src"), str(dest))
        cls.activities = {a["name"]: a["id"] for a in
                          json.loads((dest / "config" / "activities.json").read_text())["activities"]}
        cls.slot1 = json.loads((dest / "remote_slot1_map.json").read_text())
        cls.slot2 = json.loads((dest / "remote_slot2_map.json").read_text())

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def warned(self, text):
        return any(text in w for w in self.stats["warnings"])

    def test_each_remote_keeps_its_own_map(self):
        watch = self.activities["Watch TV"]
        self.assertEqual(self.slot1["activities"][watch]["Guide"]["command"], "SmartHub")
        companion = self.slot2["activities"][watch]
        self.assertEqual(companion["Guide"]["command"], "Select")
        self.assertEqual((companion["Back"]["deviceId"], companion["Back"]["command"]), ("1", "Return"))
        self.assertEqual((companion["VolumeMute"]["deviceId"], companion["VolumeMute"]["command"]), ("2", "Mute"))

    def test_activity_keys_from_the_root_map(self):
        keys = self.slot2["activities"]["-1"]
        self.assertEqual(keys["WatchTVActivity"], {"action": "activity_start", "activityId": self.activities["Watch TV"]})
        self.assertEqual(keys["MovieActivity"], {"action": "activity_start", "activityId": self.activities["Player"]})
        self.assertNotIn("MusicActivity", keys)
        self.assertTrue(self.warned("key 'MusicActivity' starts unknown activity 99999999"))
        self.assertTrue(self.warned("root map: not supported by hub_rf, dropped: 1 long/double presses"))

    def test_a_root_map_keeps_only_its_activity_keys(self):
        self.assertNotIn("-1", self.slot1["activities"])
        self.assertTrue(self.warned("stock remote 16417: 1 device command key(s) of its root map not imported"))
        self.assertEqual(set(self.slot2["activities"]["-1"]), {"WatchTVActivity", "MovieActivity"})

    def test_a_remote_beyond_the_slots_is_skipped_with_a_warning(self):
        maps = [{"remote": f"Stock remote {n}"} for n in range(mig.REMOTE_SLOT_MAX + 2)]
        warnings = []
        kept = mig.fit_stock_remote_maps(maps, warnings)
        self.assertEqual(kept, maps[:mig.REMOTE_SLOT_MAX])
        self.assertEqual(warnings, [f"stock remote {n}: only {mig.REMOTE_SLOT_MAX} RF slots are imported, map skipped"
                                    for n in (mig.REMOTE_SLOT_MAX, mig.REMOTE_SLOT_MAX + 1)])
        self.assertEqual(mig.fit_stock_remote_maps(maps[:2], warnings), maps[:2])

    def test_empty_maps_count_as_none(self):
        self.assertNotIn("-1", self.slot1["activities"])
        player = self.slot2["activities"][self.activities["Player"]]
        self.assertEqual((player["VolumeUp"]["deviceId"], player["VolumeUp"]["command"]), ("1", "VolumeUp"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
