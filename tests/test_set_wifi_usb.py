#!/usr/bin/env python3
"""tools/set_wifi_usb.py (no USB device: the Hub is a fake HarmonyHid).

- --status shows the state and exits 0 without setting anything;
- a set that the Hub reports connected with an address exits 0 and prints the address;
- a set the Hub reports failed (harmony-ng keeps its old settings) stops at once with the reason
  and exits 1, without waiting for the timeout;
- a Hub that refuses the request (HidErrorResponse) exits 1 with a clear message;
- no Hub on USB exits 1; no --ssid and no --status is a usage error;
- the password is taken from $HARMONY_WIFI_PASS and never printed; an open network sends none.
"""

from __future__ import annotations

import contextlib
import io
import os
import pathlib
import sys
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR))

from tools import set_wifi_usb  # noqa: E402
from tools.harmony_hid import HidErrorResponse  # noqa: E402


class FakeHub:
    def __init__(self, after_set: dict[str, str], refuse: bool = False):
        self.status = {"ssid": "", "connect_status": "inactive", "ip_address": "0.0.0.0",
                       "error_code": "none", "firmware": "harmony-ng"}
        self.after_set = after_set
        self.refuse = refuse
        self.sets: list[tuple[str, str, str]] = []
        self.closed = False

    def get_wifi_status(self) -> dict[str, str]:
        return dict(self.status)

    def provision_wifi(self, ssid, password, encryption="WPA2-PSK"):
        if self.refuse:
            raise HidErrorResponse("Open file '/sys/wifi/connect' for writing: the device refused it", 1, 0x0B)
        self.sets.append((ssid, password, encryption))
        self.status = dict(self.after_set)
        return True

    def close(self):
        self.closed = True


def run(hub, argv, env=None):
    out = io.StringIO()
    with mock.patch.object(set_wifi_usb, "connect", return_value=hub), \
         mock.patch.dict(os.environ, env or {}, clear=False), \
         mock.patch.object(set_wifi_usb.time, "sleep", lambda s: None), \
         contextlib.redirect_stdout(out):
        rc = set_wifi_usb.main(argv)
    return rc, out.getvalue()


class SetWifiUsb(unittest.TestCase):
    def test_status_only(self):
        hub = FakeHub({})
        rc, out = run(hub, ["--status"])
        self.assertEqual(rc, 0)
        self.assertEqual(hub.sets, [])
        self.assertIn("inactive", out)
        self.assertTrue(hub.closed)

    def test_set_connected(self):
        hub = FakeHub({"ssid": "Home", "connect_status": "connected", "ip_address": "192.0.2.20",
                       "error_code": "none"})
        rc, out = run(hub, ["--ssid", "Home"], {"HARMONY_WIFI_PASS": "secret-pass-1"})
        self.assertEqual(rc, 0)
        self.assertEqual(hub.sets, [("Home", "secret-pass-1", "WPA2-PSK")])
        self.assertIn("192.0.2.20", out)
        self.assertNotIn("secret-pass-1", out)

    def test_set_failed_reports_reason(self):
        hub = FakeHub({"ssid": "Old", "connect_status": "failed", "ip_address": "0.0.0.0",
                       "error_code": "Incorrect Wi-Fi password"})
        with mock.patch.object(set_wifi_usb.time, "time", side_effect=[0.0] + [1.0] * 5):
            rc, out = run(hub, ["--ssid", "Home", "--wait", "300"], {"HARMONY_WIFI_PASS": "wrong-pass-1"})
        self.assertEqual(rc, 1)
        self.assertIn("Incorrect Wi-Fi password", out)
        self.assertIn("previous Wi-Fi settings are kept", out)

    def test_refused_by_hub(self):
        rc, out = run(FakeHub({}, refuse=True), ["--ssid", "Home"], {"HARMONY_WIFI_PASS": "secret-pass-1"})
        self.assertEqual(rc, 1)
        self.assertIn("does not offer Wi-Fi setup over USB", out)

    def test_open_network_sends_no_password(self):
        hub = FakeHub({"ssid": "Guest", "connect_status": "connected", "ip_address": "192.0.2.21"})
        rc, _ = run(hub, ["--ssid", "Guest", "--encryption", "none"], {"HARMONY_WIFI_PASS": "ignored-pass"})
        self.assertEqual(rc, 0)
        self.assertEqual(hub.sets, [("Guest", "", "NONE")])

    def test_no_hub_and_usage(self):
        rc, out = run(None, ["--status"])
        self.assertEqual(rc, 1)
        self.assertIn("No Harmony Hub found", out)
        with self.assertRaises(SystemExit) as cm, contextlib.redirect_stderr(io.StringIO()):
            set_wifi_usb.main([])
        self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
