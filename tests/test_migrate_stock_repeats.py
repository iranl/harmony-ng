#!/usr/bin/env python3
"""tools/migrate_hub_configs.py leaves "repeats" out of stock IR commands.

Stock keeps a repeat count per device (PressMinRepeats, DefaultPressMinRepeats), not per
command, and on harmony-ng a held key repeats by itself. The stock import therefore writes
no "repeats" field and the Hub's default applies. The command's port is still written.
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


class MigrateStockRepeats(unittest.TestCase):
    def test_no_repeats_on_stock_ir_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            res = Path(tmp) / "resources"
            res.mkdir()
            (res / "DeviceList.json").write_text(json.dumps({"DevicesWithFeatures": [
                {"Device": {"Id-": 75563901, "Name": "Amp", "Transport": 1, "ControlPort": 3,
                            "PressMinRepeats": 1, "DefaultPressMinRepeats": 0},
                 "Commands": [{"Name": "VolumeUp", "KeyCode": "G:NEC:()(0x20DF40BF)():3"},
                              {"Name": "VolumeDown", "KeyCode": "G:NEC:()(0x20DFC03F)():3", "Repeats": 5}]},
                {"Device": {"Id-": 67964582, "Name": "Player", "Transport": 32},
                 "Commands": [{"Name": "Home", "KeyCode": "HID:0C:0223"}]},
            ]}), encoding="utf-8")
            out = Path(tmp) / "out"
            mig.migrate(str(res), str(out))
            devs = json.loads((out / "config" / "devices.json").read_text())["devices"]
        amp = next(d for d in devs if d["name"] == "Amp")
        self.assertEqual([c["name"] for c in amp["commands"]], ["VolumeUp", "VolumeDown"])
        for c in amp["commands"]:
            self.assertNotIn("repeats", c, f"{c['name']}: repeats written by the stock import")
            self.assertEqual(c["port"], 3)
        player = next(d for d in devs if d["name"] == "Player")
        for c in player["commands"]:
            self.assertNotIn("repeats", c)


if __name__ == "__main__":
    unittest.main(verbosity=2)
