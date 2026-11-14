#!/usr/bin/env python3
"""tools/harmony_hid.py must report a failed file write over USB HID.

A fake transport under the unchanged HarmonyHid class (no USB device): it answers the
open (0x01), chunk (0x03) and close (0x07) commands and can refuse one report write.
write_full_file() and write_simple() must return False when a report was refused,
since the file on the device is then cut short, and True for a complete write.
"""

from __future__ import annotations

import pathlib
import sys
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR))

from tools.harmony_hid import HarmonyHid  # noqa: E402


class FakeDevice:
    def __init__(self, refuse_at: int | None = None):
        self.refuse_at = refuse_at
        self.writes = 0
        self.data_reports = 0
        self.replies: list[bytes] = []

    def write_report(self, data: bytes) -> bool:
        self.writes += 1
        if self.refuse_at is not None and self.writes >= self.refuse_at:
            return False
        if data[0] == 0xFF:
            reply = bytearray(64)
            reply[:6] = bytes([0xFF, data[1], data[2], 1, 0x01, 0x07])
            self.replies.append(bytes(reply))
        else:
            self.data_reports += 1
        return True

    def read_report(self, timeout_ms: int = 5000) -> bytes:
        return self.replies.pop(0) if self.replies else b""


def hid_with(dev: FakeDevice) -> HarmonyHid:
    hid = HarmonyHid()
    hid.dev = dev
    return hid


class HidWriteResult(unittest.TestCase):
    PAYLOAD = bytes(range(256)) * 40  # 10240 bytes, 166 data reports

    def test_full_write(self):
        dev = FakeDevice()
        self.assertTrue(hid_with(dev).write_full_file("/sys/dropbear", self.PAYLOAD))
        self.assertEqual(dev.data_reports, 166)

    def test_refused_report_is_reported(self):
        # open command = write 1; the transfer stops at write 41
        dev = FakeDevice(refuse_at=41)
        hid = hid_with(dev)
        dev_writes_before_close = None

        orig_close = hid.close_file

        def close_file(handle_id, timeout_ms=10000):
            nonlocal dev_writes_before_close
            dev_writes_before_close = dev.data_reports
            dev.refuse_at = None  # let the close through
            return orig_close(handle_id, timeout_ms)

        hid.close_file = close_file
        self.assertFalse(hid.write_full_file("/sys/dropbear", self.PAYLOAD),
                         "a file cut short after a refused report write must not be reported as written")
        self.assertLess(dev_writes_before_close, 166)

    def test_simple_write_refused(self):
        for name, call in (
            ("set_tde_enable", lambda h: h.set_tde_enable(True)),
            ("provision_wifi", lambda h: h.provision_wifi("net", "secret")),
        ):
            dev = FakeDevice(refuse_at=3)  # open = 1, chunk command = 2, data report = 3
            hid = hid_with(dev)
            orig_close = hid.close_file

            def close_file(handle_id, timeout_ms=10000, dev=dev, orig_close=orig_close):
                dev.refuse_at = None
                return orig_close(handle_id, timeout_ms)

            hid.close_file = close_file
            self.assertFalse(call(hid), f"{name}() with its data report refused must return False")

    def test_simple_write_ok(self):
        self.assertTrue(hid_with(FakeDevice()).set_tde_enable(True))


if __name__ == "__main__":
    unittest.main(verbosity=2)
