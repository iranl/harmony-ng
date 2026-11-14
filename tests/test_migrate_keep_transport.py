#!/usr/bin/env python3
"""tools/migrate_hub_configs.py must keep the device fields the Hub reads (non-stock paths).

The codex folder and JSON bundle paths rebuild every device in sanitize_devices_json().
A device marked Bluetooth only by "transport": 32 must stay a Bluetooth device (no IR
port or repeats added), and a command's "isMacro" flag must survive, as in the Hub's own
sanitiser (payload/source/config_sanitize.c).
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import migrate_hub_configs as mig  # noqa: E402


class MigrateKeepTransport(unittest.TestCase):
    def test_transport_and_macro_kept(self):
        src = {"devices": [
            {"id": "2", "name": "Player", "transport": 32,
             "commands": [{"name": "Home", "keyCode": "HID:0C:0223"},
                          {"name": "Login", "raw": "MACRO:x", "isMacro": True}]},
            {"id": "3", "name": "TV", "type": "IR",
             "commands": [{"name": "PowerOn", "keyCode": "G:NEC:()(0x20DF10EF)():3"}]},
        ]}
        out = mig.sanitize_devices_json(src)["devices"]
        player, tv = out
        self.assertEqual(player.get("transport"), 32)
        self.assertEqual(player["type"], "BT")
        for c in player["commands"]:
            self.assertNotIn("port", c, "IR fields added to a Bluetooth command")
            self.assertNotIn("repeats", c)
        login = next(c for c in player["commands"] if c["name"] == "Login")
        self.assertIs(login.get("isMacro"), True)
        self.assertNotIn("isMacro", player["commands"][0])
        self.assertEqual(tv["type"], "IR")
        self.assertNotIn("transport", tv)
        self.assertEqual(tv["commands"][0]["repeats"], 3)
        self.assertEqual(tv["commands"][0]["port"], 7)


if __name__ == "__main__":
    unittest.main(verbosity=2)
