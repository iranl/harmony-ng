#!/usr/bin/env python3
"""Unit tests for the Linux libusb-1.0 fallback in tools/harmony_hid.py.

Mainline Linux kernels mark Logitech 046d:c110..c14f with HID_QUIRK_IGNORE, so no
/dev/hidraw* node is created for 046d:c129, 046d:c12b, or 046d:c132. These tests
exercise LinuxLibUsbDevice, open_linux_libusb_device(), and HarmonyHid.open() with
fake libusb calls and no USB hardware access.
"""

from __future__ import annotations

import ctypes
import pathlib
import sys
import unittest
from unittest.mock import patch

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

import tools.harmony_hid as hh  # noqa: E402


class FakeLibUsb:
    """Minimal stand-in for ctypes libusb-1.0 calls."""

    def __init__(self, descriptors: list[tuple[int, int]] | None = None):
        self.descriptors = descriptors or []
        self._dev_ptrs = (ctypes.c_void_p * (len(self.descriptors) + 1))(
            *[i + 100 for i in range(len(self.descriptors))], None
        )
        self.writes: list[tuple[int, bytes]] = []
        self.reads: list[tuple[int, bytes]] = []
        self.write_rc = 0
        self.write_short = False
        self.claim_rc = 0
        self.open_rc = 0
        self.freed_lists = 0
        self.released_ifaces = 0
        self.closed_handles = 0
        self.exited_contexts = 0

    def libusb_init(self, ctx_ref) -> int:
        ctx_ref._obj.value = 0xCAFE
        return 0

    def libusb_exit(self, _ctx) -> None:
        self.exited_contexts += 1

    def libusb_get_device_list(self, _ctx, devs_ref) -> int:
        ptr = ctypes.cast(self._dev_ptrs, ctypes.POINTER(ctypes.c_void_p))
        ctypes.memmove(ctypes.addressof(devs_ref._obj), ctypes.byref(ptr), ctypes.sizeof(ptr))
        return len(self.descriptors)

    def libusb_free_device_list(self, _devs, _unref: int) -> None:
        self.freed_lists += 1

    def libusb_get_device_descriptor(self, dev: int, desc_ref) -> int:
        idx = int(dev) - 100
        vid, pid = self.descriptors[idx]
        desc_ref._obj.idVendor = vid
        desc_ref._obj.idProduct = pid
        return 0

    def libusb_open(self, dev: int, handle_ref) -> int:
        if self.open_rc != 0:
            return self.open_rc
        handle_ref._obj.value = int(dev) + 1000
        return 0

    def libusb_close(self, _handle) -> None:
        self.closed_handles += 1

    def libusb_set_auto_detach_kernel_driver(self, _handle, _enable: int) -> int:
        return 0

    def libusb_claim_interface(self, _handle, _iface: int) -> int:
        return self.claim_rc

    def libusb_release_interface(self, _handle, _iface: int) -> int:
        self.released_ifaces += 1
        return 0

    def libusb_interrupt_transfer(self, _handle, ep: int, buf, length: int, transferred_ref, _timeout: int) -> int:
        if ep == hh.LinuxLibUsbDevice.EP_OUT:
            raw = bytes(ctypes.cast(buf, ctypes.POINTER(ctypes.c_uint8 * length)).contents)
            self.writes.append((ep, raw))
            if self.write_rc != 0:
                return self.write_rc
            transferred_ref._obj.value = 32 if self.write_short else length
            return 0
        if self.reads:
            rc, payload = self.reads.pop(0)
            if rc != 0:
                return rc
            arr = ctypes.cast(buf, ctypes.POINTER(ctypes.c_uint8 * length)).contents
            for i, b in enumerate(payload[:length]):
                arr[i] = b
            transferred_ref._obj.value = len(payload[:length])
            return 0
        return -7  # LIBUSB_ERROR_TIMEOUT


class TestLinuxLibUsbHid(unittest.TestCase):
    def _make_dev(self, fake: FakeLibUsb | None = None) -> tuple[hh.LinuxLibUsbDevice, FakeLibUsb]:
        lib = fake or FakeLibUsb()
        dev = hh.LinuxLibUsbDevice(lib, ctypes.c_void_p(0xCAFE), ctypes.c_void_p(0xBEEF), hh.REMOTE_PID)
        return dev, lib

    def test_write_report_64_and_65_bytes(self):
        dev, lib = self._make_dev()
        frame = bytes([0xFF, 0x00, 0x01, 0x00]) + bytes(60)
        self.assertTrue(dev.write_report(frame))
        self.assertTrue(dev.write_report(b"\x00" + frame))
        self.assertEqual(len(lib.writes), 2)
        self.assertEqual(lib.writes[0], (0x02, frame))
        self.assertEqual(lib.writes[1], (0x02, frame))

        with self.assertRaises(ValueError):
            dev.write_report(bytes(63))
        with self.assertRaises(ValueError):
            dev.write_report(b"\x01" + frame)

    def test_write_report_failure_and_short_transfer(self):
        dev, lib = self._make_dev()
        frame = bytes(64)
        lib.write_short = True
        self.assertFalse(dev.write_report(frame))
        lib.write_short = False
        lib.write_rc = -1
        self.assertFalse(dev.write_report(frame))

    def test_read_report_padding_and_timeout(self):
        dev, lib = self._make_dev()
        lib.reads.append((0, b"\xFF\x00\x01\x00"))
        got = dev.read_report(timeout_ms=50)
        self.assertEqual(len(got), 64)
        self.assertEqual(got[:4], b"\xFF\x00\x01\x00")
        self.assertEqual(got[4:], bytes(60))
        self.assertEqual(dev.read_report(timeout_ms=10), b"")

    def test_open_skips_other_logitech_receivers_and_matches_target_pid(self):
        # 046d:c548 (Logi Bolt) must be ignored while 046d:c12b (Harmony Remote) is opened
        fake = FakeLibUsb([(0x046D, 0xC548), (0x046D, 0xC129), (0x046D, 0xC12B)])
        with patch.object(hh, "_load_linux_libusb", return_value=fake):
            dev = hh.open_linux_libusb_device(target_pid=hh.REMOTE_PID)
            self.assertIsNotNone(dev)
            self.assertEqual(dev.pid, hh.REMOTE_PID)
            self.assertEqual(fake.freed_lists, 1)
            self.assertEqual(fake.exited_contexts, 0)
            dev.close()
            self.assertEqual(fake.released_ifaces, 1)
            self.assertEqual(fake.closed_handles, 1)
            self.assertEqual(fake.exited_contexts, 1)
            # Idempotent second close
            dev.close()
            self.assertEqual(fake.exited_contexts, 1)

    def test_open_cleans_up_context_when_no_device_matches(self):
        fake = FakeLibUsb([(0x046D, 0xC548)])
        with patch.object(hh, "_load_linux_libusb", return_value=fake):
            self.assertIsNone(hh.open_linux_libusb_device(target_pid=hh.HUB_PID))
            self.assertEqual(fake.freed_lists, 1)
            self.assertEqual(fake.exited_contexts, 1)

    def test_harmony_hid_open_prefers_hidraw_then_falls_back_to_libusb(self):
        fake = FakeLibUsb([(0x046D, 0xC12B)])
        # Stale input report drained on open, followed by ping reply
        fake.reads.append((0, b"\x00" * 64))
        fake.reads.append((-7, b""))
        fake.reads.append((-7, b""))
        fake.reads.append((0, bytes([0xFF, 0x00, 0x01, 0x00]) + bytes(60)))

        with (
            patch.object(sys, "platform", "linux"),
            patch.object(hh, "find_linux_device", return_value=None),
            patch.object(hh, "_load_linux_libusb", return_value=fake),
        ):
            hid = hh.HarmonyHid(pid=hh.REMOTE_PID)
            self.assertTrue(hid.open())
            self.assertIsInstance(hid.dev, hh.LinuxLibUsbDevice)
            # Parameterless FILE_PING (0xFF 0x00 <seq> 0x00) sent on open
            self.assertEqual(len(fake.writes), 1)
            self.assertEqual(fake.writes[0][1][:4], bytes([0xFF, 0x00, 0x01, 0x00]))
            hid.close()

        with (
            patch.object(sys, "platform", "linux"),
            patch.object(hh, "find_linux_device", return_value=None),
            patch.object(hh, "open_linux_libusb_device", return_value=None),
        ):
            self.assertFalse(hh.HarmonyHid(pid=hh.HUB_PID).open())


if __name__ == "__main__":
    unittest.main(verbosity=2)
