#!/usr/bin/env python3
"""tools/migrate_hub_configs.py leaves "repeats" out of stock IR commands and keeps the
device's PressMinRepeats.

Stock keeps the count of a press per device (PressMinRepeats), not per command, and on
harmony-ng a held key repeats by itself. The stock import therefore writes no "repeats" field;
the account's PressMinRepeats becomes the device's "pressMinRepeats" (left out when the account
has none or a value that is no count). The command's port is still written. The re-import of a
harmony-ng config (sanitize_devices_json) keeps a valid pressMinRepeats and drops a bad one.
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
                {"Device": {"Id-": 67964582, "Name": "Player", "Transport": 32, "PressMinRepeats": 3},
                 "Commands": [{"Name": "Home", "KeyCode": "HID:0C:0223"}]},
                {"Device": {"Id-": 68073190, "Name": "Box", "Transport": 1, "PressMinRepeats": None},
                 "Commands": [{"Name": "Ok", "KeyCode": "G:NEC:()(0x20DF22DD)():3"}]},
                {"Device": {"Id-": 68073191, "Name": "Disc", "Transport": 1, "PressMinRepeats": -1},
                 "Commands": [{"Name": "Ok", "KeyCode": "G:NEC:()(0x20DF22DD)():3"}]},
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
        self.assertEqual(amp.get("pressMinRepeats"), 1, "the account's PressMinRepeats")
        self.assertNotIn("pressMinRepeats", player, "a Bluetooth device sends no infrared")
        for name in ("Box", "Disc"):
            dev = next(d for d in devs if d["name"] == name)
            self.assertNotIn("pressMinRepeats", dev, f"{name}: no count in the account, none written")

    def test_reimport_keeps_press_min_repeats(self):
        base = {"name": "Dev", "type": "IR", "commands": []}
        out = mig.sanitize_devices_json({"devices": [
            dict(base, id="1", pressMinRepeats=0),
            dict(base, id="2", pressMinRepeats="2"),
            dict(base, id="3", pressMinRepeats=99),
            dict(base, id="4", pressMinRepeats="many"),
            dict(base, id="5", pressMinRepeats=-1),
            dict(base, id="6"),
        ]})["devices"]
        got = {d["id"]: d.get("pressMinRepeats", "absent") for d in out}
        self.assertEqual(got, {"1": 0, "2": 2, "3": 20, "4": "absent", "5": "absent", "6": "absent"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
