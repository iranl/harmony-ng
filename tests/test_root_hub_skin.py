#!/usr/bin/env python3
"""tools/root_hub_usb.py: the Hub's skin id is read as it reports it, and the firmware check
asks for that skin first.

The Hub reports its skin in /sys/sysinfo in hex ("0x6A" on the Creemore Hub). A plain int()
refused it, so every Hub was checked as skin 97, although the update service has a separate
image for skin 106. No USB or network: the update service is a fake.
"""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools import root_hub_usb  # noqa: E402


class FakeService:
    """query_latest_firmware stand-in: answers only for the skins it lists."""

    def __init__(self, *skins: int):
        self.skins = skins
        self.calls: list[int] = []

    def __call__(self, skin: int, silent_on_empty: bool = False):
        self.calls.append(skin)
        if skin in self.skins:
            return {"Id": "4.15.600", "URI": f"https://example.invalid/{skin}.hfw2"}
        return None


class ParseSkinTest(unittest.TestCase):
    def test_hex_as_the_hub_reports_it(self):
        self.assertEqual(root_hub_usb.parse_skin_id("0x6A"), 106)
        self.assertEqual(root_hub_usb.parse_skin_id("0x6a"), 106)
        self.assertEqual(root_hub_usb.parse_skin_id(" 0X61 "), 97)

    def test_decimal(self):
        self.assertEqual(root_hub_usb.parse_skin_id("106"), 106)
        self.assertEqual(root_hub_usb.parse_skin_id(97), 97)

    def test_missing_or_unreadable(self):
        for value in (None, "", "  ", "skin", "0x", "6A"):
            with self.subTest(value=value):
                self.assertIsNone(root_hub_usb.parse_skin_id(value))


class LatestFirmwareTest(unittest.TestCase):
    def run_quiet(self, *args):
        with contextlib.redirect_stdout(io.StringIO()):
            return root_hub_usb.latest_firmware_for_skin(*args)

    def test_reported_skin_first(self):
        service = FakeService(97, 106)
        skin, res = self.run_quiet(106, 97, service)
        self.assertEqual((skin, service.calls), (106, [106]))
        self.assertIn("/106.", res["URI"])

    def test_default_skin_when_the_reported_one_has_none(self):
        service = FakeService(97)
        skin, res = self.run_quiet(106, 97, service)
        self.assertEqual((skin, service.calls), (97, [106, 97]))
        self.assertIn("/97.", res["URI"])

    def test_default_skin_asked_once(self):
        service = FakeService()
        skin, res = self.run_quiet(97, 97, service)
        self.assertEqual((skin, res, service.calls), (97, None, [97]))


if __name__ == "__main__":
    unittest.main(verbosity=1)
