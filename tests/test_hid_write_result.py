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
        self.sent_reports: list[bytes] = []
        self.replies: list[bytes] = []

    def write_report(self, data: bytes) -> bool:
        self.writes += 1
        if self.refuse_at is not None and self.writes >= self.refuse_at:
            return False
        self.sent_reports.append(bytes(data))
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

    def test_provision_wifi_multi_packet_single_byte_counted_burst(self):
        # 65-byte payload (> 62 bytes): must be sent in ONE 0x03 burst with byte report count (1 + 2 = 3)
        dev = FakeDevice()
        hid = hid_with(dev)
        ssid = "My24GhzHomeWiFi"
        pw = " thirteenChars"  # total payload = 5 + 15 + 10 + 14 + 21 = 65 bytes
        expected = f"ssid,{ssid}\npassword,{pw}\nencryption,WPA2-PSK\n".encode("utf-8")
        self.assertEqual(len(expected), 65)

        self.assertTrue(hid.provision_wifi(ssid, pw))
        cmd_03 = [r for r in dev.sent_reports if r[0] == 0xFF and r[1] == 0x03]
        self.assertEqual(len(cmd_03), 1, "multi-frame Wi-Fi payload must be sent in a single 0x03 command")
        # Param 1 = handle_id (0x01, 0x07), Param 2 = byte total_reports (0x01, 0x03)
        self.assertEqual(cmd_03[0][4:8], bytes([0x01, 0x07, 0x01, 0x03]))

        data_frames = [r for r in dev.sent_reports if r[0] == 0x00]
        self.assertEqual(len(data_frames), 2)
        reconstructed = b"".join(df[2 : 2 + df[1]] for df in data_frames)
        self.assertEqual(reconstructed, expected)

    def test_provision_wifi_tolerates_close_exception_and_rejects_newlines(self):
        dev = FakeDevice()
        hid = hid_with(dev)
        hid.close_file = lambda *_a, **_kw: (_ for _ in ()).throw(RuntimeError("close timeout after association"))
        self.assertTrue(hid.provision_wifi("HomeNet", "secret12345"))

        dev2 = FakeDevice()
        with self.assertRaises(ValueError):
            hid_with(dev2).provision_wifi("Bad\nSSID", "pw")
        self.assertEqual(dev2.writes, 0)

    def test_write_simple_sends_all_frames_in_one_burst(self):
        dev = FakeDevice()
        payload = b"x" * 70  # 2 data frames
        self.assertTrue(hid_with(dev).write_simple("/tmp/test", payload))
        cmd_03 = [r for r in dev.sent_reports if r[0] == 0xFF and r[1] == 0x03]
        self.assertEqual(len(cmd_03), 1)
        self.assertEqual(dev.data_reports, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
