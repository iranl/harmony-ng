#!/usr/bin/env python3
"""tools/harmony_hid.py on Linux without a hidraw node (issue #241), no USB device needed.

The kernel ignores Logitech's Harmony USB range (hid_ignore()), so the Hub and the remotes get no
/dev/hidraw* node and the tools found nothing. They now open the usbfs node /dev/bus/usb/BBB/DDD.
Here sysfs, /dev/bus/usb and the usbfs ioctl are fakes; the descriptors are the Hub's own (046D:C129,
read from sysfs on a Linux PC).

- a hidraw node, when there is one, is still used as before;
- without one, the device is found in sysfs, its HID interface claimed, and the endpoints come
  from its descriptors (interrupt IN 0x81, interrupt OUT 0x02, 64 bytes);
- no device at all: open() fails with no open_error and no message (the tools' "not detected");
- reports are 64 bytes on the wire (the 0x00 report ID of the hidraw framing is dropped), a device
  without an interrupt OUT endpoint gets SET_REPORT on endpoint 0, reads return 64 bytes, a timeout
  returns nothing, a 0 ms wait is never passed to the kernel (0 = wait for ever);
- errors: no permission on the node (message with the node and the udev rule), interface in use
  (EBUSY), unsupported CPU architecture; set_wifi_usb.py prints the message;
- the ioctl numbers match linux/usbdevice_fs.h on 64-bit x86/ARM.
"""

from __future__ import annotations

import contextlib
import ctypes
import errno
import io
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR))

from tools import harmony_hid  # noqa: E402
from tools import set_wifi_usb  # noqa: E402

# The Hub's descriptors as sysfs gives them (device descriptor, then the configuration)
HUB_DESCRIPTORS = bytes.fromhex(
    "12010002000000406d0429c1971001020001"
    "0902290001010480fa"
    "09040000020300000"
    "5"
    "092100010001222100"
    "0705810340000107050203400001"
)


def descriptors_without_out_endpoint() -> bytes:
    d = bytearray(HUB_DESCRIPTORS)
    d[27 + 4] = 1  # interface (after the 18-byte device and 9-byte configuration descriptors): one endpoint
    return bytes(d[:-7])


class FakeUsbfs:
    """The usbfs ioctls of one device: records claims and transfers, answers reads from a queue."""

    def __init__(self, claim_error: int | None = None):
        self.claim_error = claim_error
        self.claimed: list[int] = []
        self.released: list[int] = []
        self.writes: list[tuple[int, bytes]] = []
        self.controls: list[tuple[int, int, int, int, bytes]] = []
        self.reads: list[bytes] = []
        self.replies: list[bytes] = []  # each write makes the next one readable (a device answer)
        self.timeouts: list[int] = []

    def __call__(self, fd, request, arg):
        if request == harmony_hid.USBDEVFS_CLAIMINTERFACE:
            if self.claim_error:
                raise OSError(self.claim_error, os.strerror(self.claim_error))
            self.claimed.append(arg.value)
            return 0
        if request == harmony_hid.USBDEVFS_RELEASEINTERFACE:
            self.released.append(arg.value)
            return 0
        if request == harmony_hid.USBDEVFS_BULK:
            self.timeouts.append(arg.timeout)
            if arg.ep & 0x80:
                if not self.reads:
                    raise OSError(errno.ETIMEDOUT, "timed out")
                data = self.reads.pop(0)[:arg.len]
                ctypes.memmove(arg.data, data, len(data))
                return len(data)
            self.writes.append((arg.ep, ctypes.string_at(arg.data, arg.len)))
            if self.replies:
                self.reads.append(self.replies.pop(0))
            return arg.len
        if request == harmony_hid.USBDEVFS_CONTROL:
            self.controls.append((arg.bRequestType, arg.bRequest, arg.wValue, arg.wIndex,
                                  ctypes.string_at(arg.data, arg.wLength)))
            return arg.wLength
        raise AssertionError(f"unexpected ioctl {request:#x}")


@unittest.skipUnless(sys.platform.startswith("linux"), "the usbfs transport is Linux only")
class UsbfsCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        self.sysfs = root / "sys"
        self.dev = root / "dev"
        self.sysfs.mkdir()
        self.dev.mkdir()
        self.fake = FakeUsbfs()
        self.patches = [
            mock.patch.object(harmony_hid, "_SYSFS_USB_DIR", str(self.sysfs)),
            mock.patch.object(harmony_hid, "_DEV_USB_DIR", str(self.dev)),
            mock.patch.object(harmony_hid, "_usbfs_ioctl", self.fake),
            mock.patch.object(harmony_hid, "find_linux_device", return_value=None),  # no hidraw node
        ]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in reversed(self.patches):
            p.stop()
        self.tmp.cleanup()

    def add_device(self, name="1-1", pid=0xC129, bus=1, devnum=2, descriptors=HUB_DESCRIPTORS,
                   mode=0o600) -> pathlib.Path:
        d = self.sysfs / name
        d.mkdir()
        (d / "idVendor").write_text("046d\n")
        (d / "idProduct").write_text(f"{pid:04x}\n")
        (d / "busnum").write_text(f"{bus}\n")
        (d / "devnum").write_text(f"{devnum}\n")
        (d / "bConfigurationValue").write_text("1\n")
        (d / "descriptors").write_bytes(descriptors)
        (self.sysfs / f"{name}:1.0").mkdir()  # an interface entry: must be skipped
        node = self.dev / f"{bus:03d}" / f"{devnum:03d}"
        node.parent.mkdir(exist_ok=True)
        node.write_bytes(b"")
        node.chmod(mode)
        return node


class TestSelection(UsbfsCase):
    def test_hidraw_node_is_still_used(self):
        node = self.add_device()
        self.patches[3].stop()
        self.patches[3] = mock.patch.object(harmony_hid, "find_linux_device", return_value=str(node))
        self.patches[3].start()
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertTrue(hid.open())
        self.assertIsInstance(hid.dev, harmony_hid.LinuxHidDevice)
        hid.close()
        self.assertEqual(self.fake.claimed, [])

    def test_usbfs_when_there_is_no_hidraw_node(self):
        self.add_device(name="3-2", bus=3, devnum=7)
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertTrue(hid.open())
        self.assertIsNone(hid.open_error)
        self.assertIsInstance(hid.dev, harmony_hid.UsbfsHidDevice)
        self.assertEqual(hid.dev.path, str(self.dev / "003" / "007"))
        self.assertEqual(hid.dev.endpoints, harmony_hid.UsbHidEndpoints(0, 0x81, 64, 0x02))
        self.assertEqual(self.fake.claimed, [0])
        hid.close()
        self.assertEqual(self.fake.released, [0])

    def test_other_product_ids_are_not_taken(self):
        self.add_device(pid=0xC52B)  # a Logitech receiver, not a Harmony device
        hid = harmony_hid.HarmonyHid()
        self.assertFalse(hid.open())
        self.assertIsNone(hid.open_error)

    def test_remote_and_any_pid(self):
        self.add_device(pid=0xC12B)
        self.assertFalse(harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID).open())
        hid = harmony_hid.HarmonyHid()
        self.assertTrue(hid.open())
        hid.close()

    def test_no_device(self):
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertFalse(hid.open())
        self.assertIsNone(hid.open_error)
        self.assertIsNone(harmony_hid.usb_open_failure_message([hid]))


class TestDescriptors(unittest.TestCase):
    def test_hub(self):
        self.assertEqual(harmony_hid.parse_hid_endpoints(HUB_DESCRIPTORS, 1),
                         harmony_hid.UsbHidEndpoints(0, 0x81, 64, 0x02))

    def test_no_out_endpoint(self):
        self.assertEqual(harmony_hid.parse_hid_endpoints(descriptors_without_out_endpoint()),
                         harmony_hid.UsbHidEndpoints(0, 0x81, 64, None))

    def test_other_configuration_is_ignored(self):
        self.assertIsNone(harmony_hid.parse_hid_endpoints(HUB_DESCRIPTORS, 2))

    def test_non_hid_interface_first(self):
        cdc = bytes.fromhex("090400000102060000" "07058303100009")  # CDC interface + interrupt IN
        d = HUB_DESCRIPTORS[:27] + cdc + HUB_DESCRIPTORS[27:]
        self.assertEqual(harmony_hid.parse_hid_endpoints(d), harmony_hid.UsbHidEndpoints(0, 0x81, 64, 0x02))

    def test_cut_descriptors(self):
        self.assertIsNone(harmony_hid.parse_hid_endpoints(HUB_DESCRIPTORS[:30]))
        self.assertIsNone(harmony_hid.parse_hid_endpoints(b""))


class TestReports(UsbfsCase):
    def open_hub(self, descriptors=HUB_DESCRIPTORS) -> harmony_hid.HarmonyHid:
        self.add_device(descriptors=descriptors)
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertTrue(hid.open())
        return hid

    def test_write_drops_the_report_id(self):
        hid = self.open_hub()
        payload = bytes(range(64))
        self.assertTrue(hid.dev.write_report(b"\x00" + payload))
        self.assertTrue(hid.dev.write_report(payload))
        self.assertEqual(self.fake.writes, [(0x02, payload), (0x02, payload)])
        with self.assertRaises(ValueError):
            hid.dev.write_report(b"\x01\x02")
        hid.close()

    def test_write_without_out_endpoint_uses_set_report(self):
        hid = self.open_hub(descriptors_without_out_endpoint())
        payload = b"\xAA" * 64
        self.assertTrue(hid.dev.write_report(b"\x00" + payload))
        self.assertEqual(self.fake.writes, [])
        self.assertEqual(self.fake.controls, [(0x21, 0x09, 0x0200, 0, payload)])
        hid.close()

    def test_read(self):
        hid = self.open_hub()
        self.fake.reads.append(b"\xFF\x10" + b"\x00" * 62)
        self.assertEqual(hid.dev.read_report(200), b"\xFF\x10" + b"\x00" * 62)
        self.assertEqual(hid.dev.read_report(200), b"")  # timeout: nothing
        hid.dev.read_report(0)
        hid.dev.read_report(-5)
        self.assertEqual(self.fake.timeouts[-2:], [1, 1])  # never 0 = wait for ever
        hid.close()

    def test_command_round_trip(self):
        hid = self.open_hub()
        self.fake.reads.append(b"\x00" * 64)  # a stale report: drained before the command
        self.fake.replies.append(b"\xFF\x10\x01" + b"\x00" * 61)
        resp = hid.command(0x10, [], timeout_ms=500)
        self.assertEqual(resp[:3], b"\xFF\x10\x01")
        self.assertEqual(len(self.fake.writes[0][1]), 64)
        hid.close()


class TestErrors(UsbfsCase):
    @unittest.skipIf(getattr(os, "geteuid", lambda: -1)() == 0, "root opens any node")
    def test_permission(self):
        node = self.add_device(mode=0)
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertFalse(hid.open())
        self.assertIsInstance(hid.open_error, PermissionError)
        msg = harmony_hid.usb_open_failure_message([hid])
        self.assertIn(str(node), msg)
        self.assertIn("may not open its device", msg)
        self.assertIn(harmony_hid.LINUX_UDEV_RULE, msg)
        self.assertIn("plugdev", msg)

    def test_busy(self):
        self.add_device()
        self.fake.claim_error = errno.EBUSY
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertFalse(hid.open())
        self.assertEqual(hid.open_error.errno, errno.EBUSY)
        self.assertIn("in use", harmony_hid.usb_open_failure_message([hid]))

    def test_busy_in_set_wifi_usb(self):
        self.add_device()
        self.fake.claim_error = errno.EBUSY
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = set_wifi_usb.main(["--status"])
        self.assertEqual(rc, 1)
        self.assertIn("in use", out.getvalue())
        self.assertNotIn("No Harmony Hub found", out.getvalue())

    def test_no_hid_interface(self):
        self.add_device(descriptors=HUB_DESCRIPTORS[:18])
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertFalse(hid.open())
        self.assertEqual(hid.open_error.errno, errno.ENODEV)
        self.assertIn("could not be opened", harmony_hid.usb_open_failure_message([hid]))

    def test_unsupported_architecture(self):
        self.add_device()
        with mock.patch.object(harmony_hid, "_usbfs_ioctl", None), \
                mock.patch.object(harmony_hid.platform, "machine", return_value="mips"):
            hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
            self.assertFalse(hid.open())
        self.assertIsInstance(hid.open_error, harmony_hid.UsbfsUnsupportedError)

    def test_permission_on_a_hidraw_node(self):
        hid = harmony_hid.HarmonyHid()
        hid.open_error = PermissionError(errno.EACCES, "Permission denied", "/dev/hidraw3")
        msg = harmony_hid.usb_open_failure_message([hid])
        self.assertIn("/dev/hidraw3", msg)
        self.assertNotIn("SUBSYSTEM", msg)  # the usb rule does not change a hidraw node

    def test_permission_wins_over_other_errors(self):
        a, b = harmony_hid.HarmonyHid(), harmony_hid.HarmonyHid()
        a.open_error = OSError(errno.EBUSY, "busy", "/dev/bus/usb/001/002")
        b.open_error = PermissionError(errno.EACCES, "Permission denied", "/dev/bus/usb/001/003")
        self.assertIn("/dev/bus/usb/001/003", harmony_hid.usb_open_failure_message([a, b]))


@unittest.skipUnless(ctypes.sizeof(ctypes.c_void_p) == 8, "64-bit layout")
class TestIoctlNumbers(unittest.TestCase):
    def test_numbers(self):
        self.assertEqual(harmony_hid.USBDEVFS_CONTROL, 0xC0185500)
        self.assertEqual(harmony_hid.USBDEVFS_BULK, 0xC0185502)
        self.assertEqual(harmony_hid.USBDEVFS_CLAIMINTERFACE, 0x8004550F)
        self.assertEqual(harmony_hid.USBDEVFS_RELEASEINTERFACE, 0x80045510)


if __name__ == "__main__":
    unittest.main(verbosity=2)
