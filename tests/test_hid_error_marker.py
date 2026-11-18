#!/usr/bin/env python3
"""tools/harmony_hid.py honours the device's error answer (byte 2 = 0xFF).

A Hub (stock hal, or harmony-ng's USB Wi-Fi setup) refuses a request with FF <cmd> FF 01 01
<code>. The client used to take byte 5 of that answer as a file handle and any FF 03 answer as
a written chunk, so a refused /sys/dropbear upload or /tde/enable was reported as done. A fake
transport (no USB device) answers like the device; checks:
- a refused open (write or read) raises HidErrorResponse with the code; a refused write chunk
  raises; a refused close returns False; set_tde_enable() returns False; write_full_file()
  raises instead of returning True;
- provision_wifi() keeps its API: True when the write is acknowledged, HidErrorResponse (a
  RuntimeError, as before for a failed open) when the Hub refuses it;
- get_wifi_status() and get_sysinfo() return {} when the file is refused;
- the client never uses sequence 127, whose success answer would carry 0xFF in byte 2;
- only the exact refusal shapes of stock hal count as errors (FF <cmd> FF 01 01 <code> and
  FF <cmd> FF 02 01 <code> 01 <handle>, also with the checksum bit): every stock success shape
  (open, write, flush, close, read, ping, reset) with any sequence 1..126 is not an error, and a
  frame with 0xFF in byte 2 but another parameter layout (a success to sequence 127) is not
  either.
"""

from __future__ import annotations

import pathlib
import sys
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR))

from tools.harmony_hid import HarmonyHid, HidErrorResponse, is_error_response  # noqa: E402


class FakeDevice:
    """Answers like hal: success FF <cmd> <seq|0x80> ..., refusal FF <cmd> FF 01 01 <code> for
    the commands in refuse (cmd -> code)."""

    def __init__(self, refuse: dict[int, int] | None = None):
        self.refuse = refuse or {}
        self.replies: list[bytes] = []
        self.seqs: list[int] = []

    def write_report(self, data: bytes) -> bool:
        if data[0] != 0xFF:
            return True
        cmd, seq = data[1], data[2]
        self.seqs.append(seq)
        reply = bytearray(64)
        if cmd in self.refuse:
            reply[:6] = bytes([0xFF, cmd, 0xFF, 0x01, 0x01, self.refuse[cmd]])
        elif cmd == 0x01:
            reply[:11] = bytes([0xFF, 0x01, seq | 0x80, 0x02, 0x01, 0x05, 0x04, 0, 0, 0, 0])
        else:
            reply[:6] = bytes([0xFF, cmd, seq | 0x80, 0x01, 0x01, 0x05])
        self.replies.append(bytes(reply))
        return True

    def read_report(self, timeout_ms: int = 5000) -> bytes:
        return self.replies.pop(0) if self.replies else b""


def hid_with(dev: FakeDevice) -> HarmonyHid:
    hid = HarmonyHid()
    hid.dev = dev
    return hid


class HidErrorMarker(unittest.TestCase):
    def test_refused_open(self):
        hid = hid_with(FakeDevice({0x01: 0x0B}))
        with self.assertRaises(HidErrorResponse) as cm:
            hid.open_file_write("/sys/dropbear", size=10)
        self.assertEqual(cm.exception.code, 0x0B)
        self.assertIsInstance(cm.exception, RuntimeError)
        with self.assertRaises(HidErrorResponse):
            hid.open_file_read("/sys/sysinfo")

    def test_refused_upload_not_reported_as_done(self):
        hid = hid_with(FakeDevice({0x01: 0x0B}))
        with self.assertRaises(HidErrorResponse):
            hid.write_full_file("/sys/dropbear", b"x" * 500)
        self.assertFalse(hid_with(FakeDevice({0x01: 0x0B})).set_tde_enable(True))
        self.assertFalse(hid_with(FakeDevice({0x03: 0x00})).set_tde_enable(True))

    def test_refused_write_chunk(self):
        hid = hid_with(FakeDevice({0x03: 0x09}))
        h = hid.open_file_write("/sys/wifi/connect", size=10)
        with self.assertRaises(HidErrorResponse) as cm:
            hid.write_file_chunks(h, b"0123456789")
        self.assertEqual(cm.exception.code, 0x09)

    def test_cut_write_answer_at_the_report_end_is_no_ack(self):
        """FF 03 in the last bytes of a report has no room for a whole answer after it: neither a
        refusal nor an acknowledgement"""
        class TailDevice(FakeDevice):
            def write_report(self, data: bytes) -> bool:
                if data[0] == 0xFF and data[1] == 0x03:
                    reply = bytearray(64)
                    reply[60:62] = b"\xFF\x03"
                    self.replies.append(bytes(reply))
                    return True
                return super().write_report(data)

        hid = hid_with(TailDevice())
        h = hid.open_file_write("/sys/wifi/connect", size=10)
        with self.assertRaises(RuntimeError) as cm:
            hid.write_file_chunks(h, b"0123456789", timeout_ms=200)
        self.assertNotIsInstance(cm.exception, HidErrorResponse)
        self.assertIn("timeout", str(cm.exception))

    def test_refused_close(self):
        self.assertFalse(hid_with(FakeDevice({0x07: 0x04})).close_file(5))
        self.assertTrue(hid_with(FakeDevice()).close_file(5))

    def test_provision_wifi_api(self):
        self.assertTrue(hid_with(FakeDevice()).provision_wifi("net", "secret123"))
        with self.assertRaises(HidErrorResponse):
            hid_with(FakeDevice({0x01: 0x0B})).provision_wifi("net", "secret123")
        with self.assertRaises(HidErrorResponse):
            hid_with(FakeDevice({0x03: 0x00})).provision_wifi("net", "secret123")

    def test_status_and_sysinfo_empty_when_refused(self):
        self.assertEqual(hid_with(FakeDevice({0x01: 0x0B})).get_wifi_status(), {})
        self.assertEqual(hid_with(FakeDevice({0x01: 0x0B})).get_sysinfo(), {})

    def test_sequence_127_never_used(self):
        dev = FakeDevice()
        hid = hid_with(dev)
        for _ in range(300):
            hid.command(0x00, [], timeout_ms=10)
        self.assertNotIn(127, dev.seqs)
        self.assertNotIn(0, dev.seqs)
        self.assertEqual(max(dev.seqs), 126)
        self.assertFalse(is_error_response(bytes([0xFF, 0x07, 126 | 0x80, 0x01, 0x01, 0x05])))
        self.assertTrue(is_error_response(bytes([0xFF, 0x07, 0xFF, 0x01, 0x01, 0x04])))


class ErrorShape(unittest.TestCase):
    def test_stock_success_answers_are_never_errors(self):
        for seq in range(1, 127):
            s = seq | 0x80
            shapes = [
                bytes([0xFF, 0x01, s, 0x02, 0x01, 0x07, 0x04, 0, 0, 0x01, 0x00]),  # open
                bytes([0xFF, 0x03, s, 0x01, 0x01, 0x07]),  # write
                bytes([0xFF, 0x05, s, 0x01, 0x01, 0x07]),  # flush
                bytes([0xFF, 0x07, s, 0x01, 0x01, 0x07]),  # close
                bytes([0xFF, 0x04, s, 0x02, 0x01, 0x07, 0x01, 0x03]),  # read
                bytes([0xFF, 0x00, s, 0x00, 0x00, 0x00]),  # ping
                bytes([0xFF, 0xFF, s, 0x00, 0x00, 0x00]),  # reset
            ]
            for r in shapes:
                self.assertFalse(is_error_response(r.ljust(64, b"\x00")), r.hex())
        # the remote's device-control answer starts F0 70
        self.assertFalse(is_error_response(bytes([0xF0, 0x70, 0xFF, 0x01, 0x01, 0x00])))

    def test_only_the_exact_refusal_shapes(self):
        for count in (0x01, 0x81):
            self.assertTrue(is_error_response(bytes([0xFF, 0x03, 0xFF, count, 0x01, 0x0B]).ljust(64, b"\x00")))
        for count in (0x02, 0x82):
            self.assertTrue(is_error_response(bytes([0xFF, 0x04, 0xFF, count, 0x01, 0x04, 0x01, 0x07]).ljust(64, b"\x00")))
        # 0xFF in byte 2 with a success layout (sequence 127): open with a size, ping
        self.assertFalse(is_error_response(bytes([0xFF, 0x01, 0xFF, 0x02, 0x01, 0x07, 0x04, 0, 0, 0, 9])))
        self.assertFalse(is_error_response(bytes([0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00])))
        self.assertFalse(is_error_response(bytes([0xFF, 0x03, 0xFF, 0x01])), "too short")


if __name__ == "__main__":
    unittest.main(verbosity=2)
