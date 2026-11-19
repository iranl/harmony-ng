#!/usr/bin/env python3
"""tools/set_wifi_usb.py (no USB device: the Hub is a fake HarmonyHid).

- --status shows the state and exits 0 without setting anything;
- a set that the Hub reports connected with an address exits 0 and prints the address;
- a set the Hub reports failed (harmony-ng keeps its old settings) stops at once with the reason
  and exits 1, without waiting for the timeout;
- a Hub that refuses the request (HidErrorResponse) exits 1 with a clear message;
- no Hub on USB exits 1; no --ssid and no --status is a usage error;
- the password is taken from $HARMONY_WIFI_PASS and never printed; an open network sends none;
- a Hub that does not answer the status yet (about a minute after Windows set the USB device up
  again) is opened again and asked again until it answers, or until the wait ends (exit 1);
- after a failed USB attempt the Hub reports "failed" while it is back on its previous network:
  shown as such, and as reconnecting while it has no network name yet;
- no traceback, a message and exit 1 when: the Hub leaves USB while the tool retries; the Hub
  never answers (then no password is asked and nothing is sent); the open or the write gets no
  answer (RuntimeError, OSError); the Hub is on USB but its Linux hidraw device may not be
  opened (permission), which harmony_hid.HarmonyHid.open() now reports in open_error.
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


class SilentHub(FakeHub):
    """Answers the status only after it was opened again `answer_after` times (as after a
    re-enumeration on Windows)."""

    def __init__(self, answer_after: int):
        super().__init__({})
        self.status.update({"ssid": "Home", "connect_status": "connected", "ip_address": "192.0.2.20"})
        self.answer_after = answer_after
        self.reopens = 0
        self.closes = 0

    def get_wifi_status(self):
        return dict(self.status) if self.reopens >= self.answer_after else {}

    def close(self):
        self.closes += 1
        super().close()


def reopening(hub, clock_step: float):
    """Patches for read_status(): connect() opens the same fake again, a clock moving clock_step
    seconds per reading, and no real sleeping."""
    def reconnect():
        hub.reopens += 1
        return hub
    now = [0.0]

    def clock():
        now[0] += clock_step
        return now[0]
    return [mock.patch.object(set_wifi_usb, "connect", side_effect=reconnect),
            mock.patch.object(set_wifi_usb.time, "time", side_effect=clock),
            mock.patch.object(set_wifi_usb.time, "sleep", lambda s: None)]


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

    def test_status_answer_after_reopening(self):
        hub = SilentHub(answer_after=3)
        out = io.StringIO()
        with contextlib.ExitStack() as stack:
            for p in reopening(hub, clock_step=1.0):
                stack.enter_context(p)
            stack.enter_context(contextlib.redirect_stdout(out))
            got, st = set_wifi_usb.read_status(hub, wait_s=60)
        self.assertIs(got, hub)
        self.assertEqual(st.get("connect_status"), "connected")
        self.assertEqual(hub.reopens, 3)
        self.assertEqual(hub.closes, 3, "the device is closed before each new open")
        self.assertIn("does not answer yet", out.getvalue())

    def test_status_gives_up_after_the_wait(self):
        hub = SilentHub(answer_after=10**6)
        out = io.StringIO()
        with contextlib.ExitStack() as stack:
            for p in reopening(hub, clock_step=10.0):
                stack.enter_context(p)
            stack.enter_context(contextlib.redirect_stdout(out))
            rc = set_wifi_usb.main(["--status"])
        self.assertEqual(rc, 1)
        self.assertIn("did not answer", out.getvalue())
        self.assertLessEqual(hub.reopens, 7, "stops at the wait, about 60 s")

    def test_failed_attempt_back_on_previous_network(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            set_wifi_usb.show_status({"ssid": "Old", "connect_status": "failed", "ip_address": "192.0.2.5",
                                      "error_code": "Incorrect Wi-Fi password"})
        self.assertIn("on its previous network (the last USB attempt failed)", out.getvalue())
        self.assertIn("Network:  Old", out.getvalue())
        self.assertIn("Last USB attempt failed: Incorrect Wi-Fi password", out.getvalue())

    def test_failed_attempt_while_reconnecting(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            set_wifi_usb.show_status({"ssid": "", "connect_status": "failed", "ip_address": "0.0.0.0",
                                      "error_code": "Connection timed out"})
        self.assertIn("(reconnecting to the previous network)", out.getvalue())
        self.assertIn("State:    failed", out.getvalue())


class CleanFailures(unittest.TestCase):
    def run_main(self, argv, connect, env=None, clock_step=10.0):
        out = io.StringIO()
        now = [0.0]

        def clock():
            now[0] += clock_step
            return now[0]
        with mock.patch.object(set_wifi_usb, "connect", side_effect=connect), \
             mock.patch.object(set_wifi_usb.time, "time", side_effect=clock), \
             mock.patch.object(set_wifi_usb.time, "sleep", lambda s: None), \
             mock.patch.dict(os.environ, env or {}, clear=False), \
             mock.patch("getpass.getpass", side_effect=AssertionError("password asked")), \
             contextlib.redirect_stdout(out):
            rc = set_wifi_usb.main(argv)
        return rc, out.getvalue()

    def test_device_gone_during_retry(self):
        hub = SilentHub(answer_after=10**6)
        calls = iter([hub])
        rc, out = self.run_main(["--ssid", "Home"], lambda: next(calls, None))
        self.assertEqual(rc, 1)
        self.assertIn("no longer on USB", out)
        self.assertEqual(hub.sets, [])

    def test_silent_hub_no_password_asked(self):
        hub = SilentHub(answer_after=10**6)
        rc, out = self.run_main(["--ssid", "Home"], lambda: hub)
        self.assertEqual(rc, 1)
        self.assertIn("did not answer", out)
        self.assertEqual(hub.sets, [], "settings were sent to a Hub that never answered")

    def test_open_timeout_is_a_clean_error(self):
        class Timeout(FakeHub):
            def provision_wifi(self, ssid, password, encryption="WPA2-PSK"):
                raise RuntimeError("Open file '/sys/wifi/connect' failed: response timeout")
        for exc_hub in (Timeout({}),):
            rc, out = self.run_main(["--ssid", "Home"], lambda: exc_hub, {"HARMONY_WIFI_PASS": "secret-pass-1"})
            self.assertEqual(rc, 1)
            self.assertIn("did not answer the request", out)

        class Gone(FakeHub):
            def provision_wifi(self, ssid, password, encryption="WPA2-PSK"):
                raise OSError("HID report write failed")
        rc, out = self.run_main(["--ssid", "Home"], lambda: Gone({}), {"HARMONY_WIFI_PASS": "secret-pass-1"})
        self.assertEqual(rc, 1)
        self.assertIn("did not answer the request", out)

    def test_permission_denied_on_linux(self):
        class Denied:
            def __init__(self, pid=None):
                self.open_error = PermissionError(13, "Permission denied", "/dev/hidraw3")

            def open(self):
                return False
        out = io.StringIO()
        with mock.patch.object(set_wifi_usb, "HarmonyHid", Denied), contextlib.redirect_stdout(out):
            rc = set_wifi_usb.main(["--status"])
        self.assertEqual(rc, 1)
        self.assertIn("may not open its device", out.getvalue())
        self.assertNotIn("No Harmony Hub found", out.getvalue())

    @unittest.skipUnless(sys.platform.startswith("linux") and os.geteuid() != 0, "Linux, not root")
    def test_harmony_hid_reports_the_open_error(self):
        from tools import harmony_hid
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            node = pathlib.Path(d) / "hidraw9"
            node.write_text("")
            node.chmod(0)
            hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
            with mock.patch.object(harmony_hid, "find_linux_device", return_value=str(node)):
                self.assertFalse(hid.open())
            self.assertIsInstance(hid.open_error, PermissionError)


if __name__ == "__main__":
    unittest.main(verbosity=2)
