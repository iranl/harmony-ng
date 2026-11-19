#!/usr/bin/env python3
"""tools/harmony_hid.py on Linux without a hidraw node (issue #241), no USB device needed.

The kernel ignores Logitech's Harmony USB range (hid_ignore()), so the Hub and the remotes get no
/dev/hidraw* node and the tools found nothing. They now open the usbfs node /dev/bus/usb/BBB/DDD.
Here sysfs, /dev/bus/usb, the usbfs ioctls and the wait are fakes; the descriptors are the Hub's
own (046D:C129, read from sysfs on a Linux PC).

- a hidraw node, when there is one, is still used as before;
- without one, the device is found in sysfs, its HID interface claimed (whatever its number), and
  the endpoints come from its descriptors (interrupt IN 0x81, interrupt OUT 0x02, 64 bytes);
  bulk endpoints are not taken;
- no device at all: open() fails with no open_error and no message (the tools' "not detected");
- reports are 64 bytes on the wire (the 0x00 report ID of the hidraw framing is dropped), each one
  an asynchronous URB; a device without an interrupt OUT endpoint gets SET_REPORT on endpoint 0;
  reads return at most 64 bytes; a short or failed write is a failure;
- a URB still running at the deadline is cancelled and collected: a report that completes during
  the cancel is returned, otherwise "no report"; Ctrl-C during the wait cancels and collects the URB
  too, then goes through;
- a device that left the bus (ENODEV) is marked gone: reads stop at once and command() stops
  waiting; other transfer errors are kept, reported once, and do not busy-loop;
- errors on open: no permission (message with the node and both udev rules, a hidraw node gets its
  own text), interface in use (EBUSY, no fd left open), no HID interface, unsupported CPU
  architecture, other claim errors keep the node; set_wifi_usb.py prints the message;
- the struct layouts and ioctl numbers match linux/usbdevice_fs.h on 64-bit; the real ioctl
  wrapper reaches the kernel (ENOTTY on /dev/null).
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
import time
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR))

from tools import harmony_hid  # noqa: E402
from tools import set_wifi_usb  # noqa: E402

LINUX = sys.platform.startswith("linux")

# The Hub's descriptors as sysfs gives them (device descriptor, then the configuration)
HUB_DESCRIPTORS = bytes.fromhex(
    "12010002000000406d0429c1971001020001"
    "0902290001010480fa"
    "090400000203000005"
    "092100010001222100"
    "0705810340000107050203400001"
)


def descriptors_without_out_endpoint() -> bytes:
    d = bytearray(HUB_DESCRIPTORS)
    d[27 + 4] = 1  # interface (after the 18-byte device and 9-byte configuration descriptors): one endpoint
    return bytes(d[:-7])


def descriptors_interface_1_bulk_out() -> bytes:
    d = bytearray(HUB_DESCRIPTORS)
    d[27 + 2] = 1  # bInterfaceNumber 1
    d[-4] = 0x02   # the OUT endpoint becomes bulk: not an interrupt endpoint
    return bytes(d)


def descriptors_in_size(size: int) -> bytes:
    d = bytearray(HUB_DESCRIPTORS)
    d[45 + 4] = size & 0xFF  # wMaxPacketSize of the IN endpoint
    d[45 + 5] = size >> 8
    return bytes(d)


class FakeUsbfs:
    """The usbfs ioctls of one device. One URB in flight at a time; a submitted IN URB completes
    when a report is queued, an OUT URB at once. Records claims, writes and control transfers."""

    def __init__(self):
        self.claim_error: int | None = None
        self.claimed: list[int] = []
        self.released: list[int] = []
        self.writes: list[tuple[int, bytes]] = []
        self.controls: list[tuple[int, int, int, int, bytes]] = []
        self.reads: list[bytes] = []
        self.replies: list[bytes] = []  # each write makes the next one readable (a device answer)
        self.late: bytes | None = None  # a report that completes while the URB is being cancelled
        self.urb_error: int | None = None  # IN URBs complete with -errno
        self.out_error: int | None = None  # OUT URBs complete with -errno
        self.write_result: int | None = None  # bytes an OUT URB reports moved (short write)
        self.reap_error: int | None = None  # REAPURB* fail with this errno (device gone)
        self.pending = None
        self.done = None
        self.submits = 0
        self.discards = 0
        self.waits: list[float] = []

    def _complete_in(self):
        if self.urb_error:
            self.done = ("err", self.urb_error)
        elif self.reads:
            self.done = ("data", self.reads.pop(0))

    def __call__(self, fd, request, arg):
        if request == harmony_hid.USBDEVFS_CLAIMINTERFACE:
            if self.claim_error:
                raise OSError(self.claim_error, os.strerror(self.claim_error))
            self.claimed.append(arg.value)
            return 0
        if request == harmony_hid.USBDEVFS_RELEASEINTERFACE:
            self.released.append(arg.value)
            return 0
        if request == harmony_hid.USBDEVFS_SUBMITURB:
            assert self.pending is None, "a second URB while one is in flight"
            assert arg.type == harmony_hid.USBDEVFS_URB_TYPE_INTERRUPT
            self.submits += 1
            self.pending, self.done = arg, None
            if arg.endpoint & 0x80:
                self._complete_in()
            else:
                self.writes.append((arg.endpoint, ctypes.string_at(arg.buffer, arg.buffer_length)))
                if self.replies:
                    self.reads.append(self.replies.pop(0))
                if self.out_error:
                    self.done = ("err", self.out_error)
                else:
                    self.done = ("moved", arg.buffer_length if self.write_result is None else self.write_result)
            return 0
        if request == harmony_hid.USBDEVFS_DISCARDURB:
            self.discards += 1
            assert arg is self.pending
            if self.done is None and self.late is not None:
                self.done, self.late = ("data", self.late), None
            if self.done is not None:
                raise OSError(errno.EINVAL, "already completed")
            self.done = ("err", errno.ENOENT)
            return 0
        if request in (harmony_hid.USBDEVFS_REAPURB, harmony_hid.USBDEVFS_REAPURBNDELAY):
            if self.reap_error:
                raise OSError(self.reap_error, os.strerror(self.reap_error))
            if self.done is None:
                assert request == harmony_hid.USBDEVFS_REAPURBNDELAY, "blocking reap would hang"
                raise OSError(errno.EAGAIN, "nothing completed")
            urb, (kind, val) = self.pending, self.done
            if kind == "data":
                data = val[:urb.buffer_length]
                ctypes.memmove(urb.buffer, data, len(data))
                urb.status, urb.actual_length = 0, len(data)
            elif kind == "moved":
                urb.status, urb.actual_length = 0, val
            else:
                urb.status, urb.actual_length = -val, 0
            arg.value = ctypes.addressof(urb)
            self.pending, self.done = None, None
            return 0
        if request == harmony_hid.USBDEVFS_CONTROL:
            self.controls.append((arg.bRequestType, arg.bRequest, arg.wValue, arg.wIndex,
                                  ctypes.string_at(arg.data, arg.wLength)))
            return arg.wLength
        raise AssertionError(f"unexpected ioctl {request:#x}")

    def wait(self, fd, timeout_s):
        """select(): ready when the URB has completed; otherwise the time is up."""
        self.waits.append(timeout_s)
        return self.done is not None


@unittest.skipUnless(LINUX, "the usbfs transport is Linux only")
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
            mock.patch.object(harmony_hid, "_usbfs_wait", self.fake.wait),
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

    def open_hub(self, descriptors=HUB_DESCRIPTORS) -> harmony_hid.HarmonyHid:
        self.add_device(descriptors=descriptors)
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertTrue(hid.open())
        return hid


class TestSelection(UsbfsCase):
    def test_hidraw_node_is_still_used(self):
        node = self.add_device()
        self.patches[4].stop()
        self.patches[4] = mock.patch.object(harmony_hid, "find_linux_device", return_value=str(node))
        self.patches[4].start()
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

    def test_interface_number_is_claimed(self):
        hid = self.open_hub(descriptors_interface_1_bulk_out())
        self.assertEqual(self.fake.claimed, [1])
        self.assertIsNone(hid.dev.endpoints.ep_out)  # bulk OUT is not an interrupt endpoint
        hid.close()
        self.assertEqual(self.fake.released, [1])

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

    def test_interface_1_and_bulk_out(self):
        self.assertEqual(harmony_hid.parse_hid_endpoints(descriptors_interface_1_bulk_out()),
                         harmony_hid.UsbHidEndpoints(1, 0x81, 64, None))

    def test_other_configuration_is_ignored(self):
        self.assertIsNone(harmony_hid.parse_hid_endpoints(HUB_DESCRIPTORS, 2))

    def test_non_hid_interface_first(self):
        cdc = bytes.fromhex("090400000102060000" "07058303100009")  # CDC interface 0 + interrupt IN
        hid = bytearray(HUB_DESCRIPTORS[27:])
        hid[2] = 1  # the HID interface is number 1
        self.assertEqual(harmony_hid.parse_hid_endpoints(HUB_DESCRIPTORS[:27] + cdc + bytes(hid)),
                         harmony_hid.UsbHidEndpoints(1, 0x81, 64, 0x02))

    def test_cut_descriptors(self):
        self.assertIsNone(harmony_hid.parse_hid_endpoints(HUB_DESCRIPTORS[:30]))
        self.assertIsNone(harmony_hid.parse_hid_endpoints(b""))


class TestReports(UsbfsCase):
    def test_write_drops_the_report_id(self):
        hid = self.open_hub()
        payload = bytes(range(64))
        self.assertTrue(hid.dev.write_report(b"\x00" + payload))
        self.assertTrue(hid.dev.write_report(payload))
        self.assertEqual(self.fake.writes, [(0x02, payload), (0x02, payload)])
        with self.assertRaises(ValueError):
            hid.dev.write_report(b"\x01\x02")
        hid.close()

    def test_short_or_failed_write_is_a_failure(self):
        hid = self.open_hub()
        self.fake.write_result = 10
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(hid.dev.write_report(bytes(64)))
            self.fake.write_result = None
            self.fake.out_error = errno.EPROTO
            self.assertFalse(hid.dev.write_report(bytes(64)))
        self.assertEqual(hid.dev.last_error.errno, errno.EPROTO)
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
        self.assertEqual(hid.dev.read_report(200), b"")  # nothing came: no report
        self.assertIsNone(self.fake.pending)  # the cancelled URB was collected
        self.assertIsNone(hid.dev.last_error)  # a timeout is not an error
        self.fake.waits.clear()
        hid.dev.read_report(0)
        hid.dev.read_report(-5)
        self.assertTrue(self.fake.waits and all(0 < w <= 0.001 for w in self.fake.waits))  # never "for ever"
        hid.close()

    def test_read_is_cut_to_64_bytes(self):
        hid = self.open_hub(descriptors_in_size(128))
        self.fake.reads.append(bytes(range(100)))
        self.assertEqual(hid.dev.read_report(200), bytes(range(64)))
        hid.close()

    def test_report_completing_during_the_cancel_is_kept(self):
        hid = self.open_hub()
        discards = self.fake.discards  # open() drains once
        self.fake.late = b"\xFF\x04" + b"\x01" * 62
        self.assertEqual(hid.dev.read_report(50), b"\xFF\x04" + b"\x01" * 62)
        self.assertEqual(self.fake.discards, discards + 1)
        hid.close()

    def test_ctrl_c_cancels_and_collects_the_urb(self):
        hid = self.open_hub()

        def interrupted(fd, timeout_s):
            raise KeyboardInterrupt
        hid.dev._wait = interrupted
        discards = self.fake.discards
        with self.assertRaises(KeyboardInterrupt):
            hid.dev.read_report(60000)
        self.assertIsNone(self.fake.pending)
        self.assertEqual(self.fake.discards, discards + 1)
        hid.close()

    def test_command_round_trip(self):
        hid = self.open_hub()
        self.fake.reads.append(b"\x00" * 64)  # a stale report: drained before the command
        self.fake.replies.append(b"\xFF\x10\x01" + b"\x00" * 61)
        resp = hid.command(0x10, [], timeout_ms=500)
        self.assertEqual(resp[:3], b"\xFF\x10\x01")
        self.assertEqual(len(self.fake.writes[0][1]), 64)
        hid.close()


class TestTransferErrors(UsbfsCase):
    def test_device_gone_stops_the_wait(self):
        hid = self.open_hub()
        self.fake.reap_error = errno.ENODEV
        err = io.StringIO()
        t0 = time.monotonic()
        with contextlib.redirect_stderr(err):
            self.assertEqual(hid.dev.read_report(5000), b"")
            self.assertTrue(hid.dev.gone)
            submits = self.fake.submits
            with self.assertRaises(OSError):  # the write fails at once: nothing waits 5 s
                hid.command(0x10, [], timeout_ms=5000)
        self.assertLess(time.monotonic() - t0, 1.0)
        self.assertEqual(self.fake.submits, submits)  # nothing more sent to a device that is gone
        self.assertIn("left USB", err.getvalue())
        hid.close()

    def test_endpoint_error_is_kept_and_does_not_spin(self):
        hid = self.open_hub()
        self.fake.urb_error = errno.EPIPE
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            t0 = time.monotonic()
            self.assertEqual(hid.command(0x10, [], timeout_ms=300), b"")
            elapsed = time.monotonic() - t0
        self.assertEqual(hid.dev.last_error.errno, errno.EPIPE)
        self.assertFalse(hid.dev.gone)
        self.assertEqual(err.getvalue().count("USB transfer error"), 1)  # said once
        self.assertLess(self.fake.submits, 100)  # about one per 10 ms, not a busy loop
        self.assertGreaterEqual(elapsed, 0.25)
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
        self.assertIn(harmony_hid.LINUX_UDEV_RULE_PLUGDEV, msg)
        self.assertNotIn("GROUP", harmony_hid.LINUX_UDEV_RULE)  # a missing group drops the whole line

    def test_busy_leaves_no_fd_open(self):
        self.add_device()
        self.fake.claim_error = errno.EBUSY
        before = len(os.listdir("/proc/self/fd"))
        for _ in range(5):
            hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
            self.assertFalse(hid.open())
        self.assertEqual(len(os.listdir("/proc/self/fd")), before)
        self.assertEqual(hid.open_error.errno, errno.EBUSY)
        msg = harmony_hid.usb_open_failure_message([hid])
        self.assertIn("in use", msg)
        self.assertIn("virtual machine", msg)

    def test_other_claim_error_names_the_node(self):
        node = self.add_device()
        self.fake.claim_error = errno.EHOSTUNREACH
        hid = harmony_hid.HarmonyHid(pid=harmony_hid.HUB_PID)
        self.assertFalse(hid.open())
        self.assertIn(str(node), harmony_hid.usb_open_failure_message([hid]))

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


class TestMessages(unittest.TestCase):
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
class TestAbi(unittest.TestCase):
    """Values of linux/usbdevice_fs.h on x86-64 (a C program built against the header prints them)."""

    def test_ioctl_numbers(self):
        self.assertEqual(harmony_hid.USBDEVFS_CONTROL, 0xC0185500)
        self.assertEqual(harmony_hid.USBDEVFS_SUBMITURB, 0x8038550A)
        self.assertEqual(harmony_hid.USBDEVFS_DISCARDURB, 0x550B)
        self.assertEqual(harmony_hid.USBDEVFS_REAPURB, 0x4008550C)
        self.assertEqual(harmony_hid.USBDEVFS_REAPURBNDELAY, 0x4008550D)
        self.assertEqual(harmony_hid.USBDEVFS_CLAIMINTERFACE, 0x8004550F)
        self.assertEqual(harmony_hid.USBDEVFS_RELEASEINTERFACE, 0x80045510)

    def test_struct_layouts(self):
        ctrl = harmony_hid._UsbfsCtrl
        self.assertEqual([getattr(ctrl, f).offset for f in
                          ("bRequestType", "bRequest", "wValue", "wIndex", "wLength", "timeout", "data")],
                         [0, 1, 2, 4, 6, 8, 16])
        self.assertEqual(ctypes.sizeof(ctrl), 24)
        urb = harmony_hid._UsbfsUrb
        self.assertEqual([getattr(urb, f).offset for f in
                          ("type", "endpoint", "status", "flags", "buffer", "buffer_length", "actual_length",
                           "start_frame", "number_of_packets", "error_count", "signr", "usercontext")],
                         [0, 1, 4, 8, 16, 24, 28, 32, 36, 40, 44, 48])
        self.assertEqual(ctypes.sizeof(urb), 56)


@unittest.skipUnless(LINUX, "libc ioctl on Linux")
class TestRealIoctl(unittest.TestCase):
    def test_reaches_the_kernel(self):
        fd = os.open("/dev/null", os.O_RDWR)
        try:
            for request, arg in ((harmony_hid.USBDEVFS_CLAIMINTERFACE, ctypes.c_uint(0)),
                                 (harmony_hid.USBDEVFS_REAPURBNDELAY, ctypes.c_void_p()),
                                 (harmony_hid.USBDEVFS_CONTROL, harmony_hid._UsbfsCtrl()),
                                 (harmony_hid.USBDEVFS_SUBMITURB, harmony_hid._UsbfsUrb())):
                with self.assertRaises(OSError) as cm:
                    harmony_hid.usbfs_ioctl(fd, request, arg)
                self.assertEqual(cm.exception.errno, errno.ENOTTY)
        finally:
            os.close(fd)

    def test_device_uses_the_real_ioctl_by_default(self):
        fd = os.open("/dev/null", os.O_RDWR)
        dev = harmony_hid.UsbfsHidDevice(fd, "/dev/null", harmony_hid.UsbHidEndpoints(0, 0x81, 64, 0x02))
        with self.assertRaises(OSError) as cm:
            dev.claim()
        self.assertEqual(cm.exception.errno, errno.ENOTTY)
        dev.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
