#!/usr/bin/env python3
"""Clean-room Logitech Harmony USB HID Client.

Communicates with Logitech Harmony Hub (046D:C129) and the touchscreen remotes (046D:C12B)
over raw USB HID without third-party dependencies.

Supported OS:
- Windows: Native ctypes (setupapi.dll + hid.dll + kernel32.dll)
- Linux: Native /dev/hidraw* when the kernel made one, else the USB device node
  (/dev/bus/usb/BBB/DDD) through usbfs ioctls: the kernel ignores Logitech's Harmony USB range
  (046D:C110-C14F, hid_ignore() in drivers/hid/hid-quirks.c), so normally no hidraw node exists
- macOS: Native ctypes (IOKit.framework + CoreFoundation.framework)

License: MIT (Harmony Hub Control Project)
"""

from __future__ import annotations

import ctypes
import errno
import os
import platform
import struct
import sys
import time
from dataclasses import dataclass
from typing import Optional, Tuple

HARMONY_VID = 0x046D
HUB_PID = 0xC129
# 046D:C132 is the Harmony Pro 2400 Hub in the docs, but remote/setup_remote.py and
# tools/usb_state.py also accept it for a remote, and which Pro device enumerates with it is not
# verified on hardware. So the USB id is not trusted to tell the model: the firmware flasher checks
# the skin the device reports, and the scripts the root tools stage exit on the wrong uname -m.
HUB_PRO_PID = 0xC132
REMOTE_PID = 0xC12B
ALL_HUB_PIDS = (HUB_PID, HUB_PRO_PID)
ALL_HARMONY_PIDS = (HUB_PID, HUB_PRO_PID, REMOTE_PID)

REPORT_SIZE = 65  # 1 byte report ID (0x00) + 64 bytes payload


class HidErrorResponse(RuntimeError):
    """The device refused a request: its answer has 0xFF in byte 2 (FF <cmd> FF ... <code>)."""

    def __init__(self, message: str, cmd: int, code: Optional[int]):
        super().__init__(message)
        self.cmd = cmd
        self.code = code


def is_error_response(resp: bytes) -> bool:
    """True only for the exact refusal stock hal builds (libhal_send_error_response, Hub hal
    0x418f10; the remote's hal FUN_0000f010 builds the same): FF <cmd> FF, a parameter count of
    1 or 2 (bit 7 set when a checksum follows), first parameter one byte (TL 01) holding the
    code, the second (count 2) one byte too: FF <cmd> FF 01 01 <code> or
    FF <cmd> FF 02 01 <code> 01 <handle>. harmony-ng's USB Wi-Fi setup sends the first form.
    A stock success answer (libhal_send_success_response 0x418990) carries the request's 7-bit
    sequence with bit 7 set in byte 2, so it is 0xFF only for sequence 127, which this client
    never uses (HarmonyHid._next_seq); the shape check is a second guard against reading a
    success as a refusal."""
    if len(resp) < 6 or resp[0] != 0xFF or resp[2] != 0xFF:
        return False
    count = resp[3] & 0x3F
    if count == 1:
        return resp[4] == 0x01
    if count == 2:
        return len(resp) >= 8 and resp[4] == 0x01 and resp[6] == 0x01
    return False


def error_code(resp: bytes) -> Optional[int]:
    """The error code of a refusal (is_error_response), None for any other answer."""
    return resp[5] if is_error_response(resp) else None


def _refused(what: str, resp: bytes) -> HidErrorResponse:
    code = error_code(resp)
    reason = {0x04: "unknown handle", 0x0B: "not offered by this device", 0x0C: "request not understood",
              0x0D: "too many open files"}.get(code, "refused") if code is not None else "refused"
    shown = f"0x{code:02X}" if code is not None else "none"
    return HidErrorResponse(f"{what}: the device refused it ({reason}, code {shown})", resp[1], code)


# ==============================================================================
# Protocol Framing Helpers
# ==============================================================================

def encode_param_byte(val: int) -> bytes:
    return bytes([0x01, val & 0xFF])


def encode_param_word(val: int) -> bytes:
    return bytes([0x02, val & 0xFF, (val >> 8) & 0xFF])


def encode_param_word_be(val: int) -> bytes:
    return bytes([0x02, (val >> 8) & 0xFF, val & 0xFF])


def encode_param_dword_be(val: int) -> bytes:
    return bytes([
        0x04,
        (val >> 24) & 0xFF,
        (val >> 16) & 0xFF,
        (val >> 8) & 0xFF,
        val & 0xFF,
    ])


def encode_param_string(s: str) -> bytes:
    return b"\x80" + s.encode("ascii") + b"\x00"


def build_primary_frames(cmd_id: int, seq: int, params: list[bytes]) -> list[bytes]:
    """Build primary command frames (64 bytes each)."""
    stream = bytearray([0xFF, cmd_id & 0xFF, seq & 0x7F, len(params) & 0x3F])
    for p in params:
        stream.extend(p)

    frames = []
    for off in range(0, len(stream), 64):
        chunk = stream[off : off + 64]
        frame = bytearray(64)
        frame[: len(chunk)] = chunk
        frames.append(bytes(frame))
    return frames


def build_data_frames(data: bytes, chunk_size: int = 62) -> list[bytes]:
    """Slice raw binary data into secondary data frames (64 bytes each)."""
    frames = []
    for off in range(0, len(data), chunk_size):
        chunk = data[off : off + chunk_size]
        frame = bytearray(64)
        frame[0] = 0x00
        frame[1] = len(chunk)
        frame[2 : 2 + len(chunk)] = chunk
        frames.append(bytes(frame))
    if not frames:
        frames.append(bytes(64))
    return frames


# ==============================================================================
# Windows WinHID Transport (ctypes)
# ==============================================================================

if sys.platform == "win32":
    from ctypes import wintypes

    class GUID(ctypes.Structure):
        _fields_ = [
            ("Data1", wintypes.DWORD),
            ("Data2", wintypes.WORD),
            ("Data3", wintypes.WORD),
            ("Data4", wintypes.BYTE * 8),
        ]

    class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
        _fields_ = [
            ("cbSize", wintypes.DWORD),
            ("InterfaceClassGuid", GUID),
            ("Flags", wintypes.DWORD),
            ("Reserved", ctypes.POINTER(wintypes.ULONG)),
        ]

    class OVERLAPPED(ctypes.Structure):
        _fields_ = [
            ("Internal", ctypes.POINTER(wintypes.ULONG)),
            ("InternalHigh", ctypes.POINTER(wintypes.ULONG)),
            ("Offset", wintypes.DWORD),
            ("OffsetHigh", wintypes.DWORD),
            ("hEvent", wintypes.HANDLE),
        ]

    kernel32 = ctypes.windll.kernel32
    setupapi = ctypes.windll.setupapi
    hid_dll = ctypes.windll.hid

    # Win32 SetupAPI prototypes (critical on 64-bit Windows to avoid 32-bit handle truncation)
    setupapi.SetupDiGetClassDevsW.restype = wintypes.HANDLE
    setupapi.SetupDiGetClassDevsW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        wintypes.HWND,
        wintypes.DWORD,
    ]
    setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
    setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.c_void_p,
    ]
    setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
    setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.c_void_p,
    ]
    setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL
    setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wintypes.HANDLE]

    # HID DLL prototypes
    hid_dll.HidD_GetHidGuid.restype = None
    hid_dll.HidD_GetHidGuid.argtypes = [ctypes.c_void_p]
    hid_dll.HidD_FlushQueue.restype = wintypes.BOOL
    hid_dll.HidD_FlushQueue.argtypes = [wintypes.HANDLE]
    hid_dll.HidD_SetNumInputBuffers.restype = wintypes.BOOL
    hid_dll.HidD_SetNumInputBuffers.argtypes = [wintypes.HANDLE, wintypes.ULONG]

    # Kernel32 prototypes
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = [
        ctypes.c_wchar_p,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.c_void_p,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    kernel32.CreateEventW.restype = wintypes.HANDLE
    kernel32.CreateEventW.argtypes = [
        ctypes.c_void_p,
        wintypes.BOOL,
        wintypes.BOOL,
        ctypes.c_wchar_p,
    ]
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.ResetEvent.restype = wintypes.BOOL
    kernel32.ResetEvent.argtypes = [wintypes.HANDLE]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WriteFile.restype = wintypes.BOOL
    kernel32.WriteFile.argtypes = [
        wintypes.HANDLE,
        ctypes.c_char_p,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.c_void_p,
    ]
    kernel32.ReadFile.restype = wintypes.BOOL
    kernel32.ReadFile.argtypes = [
        wintypes.HANDLE,
        ctypes.c_char_p,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.c_void_p,
    ]
    kernel32.GetOverlappedResult.restype = wintypes.BOOL
    kernel32.GetOverlappedResult.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.BOOL,
    ]
    kernel32.CancelIo.restype = wintypes.BOOL
    kernel32.CancelIo.argtypes = [wintypes.HANDLE]
    kernel32.GetLastError.restype = wintypes.DWORD
    kernel32.GetLastError.argtypes = []

    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 0x00000001
    FILE_SHARE_WRITE = 0x00000002
    OPEN_EXISTING = 3
    FILE_FLAG_OVERLAPPED = 0x40000000
    INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
    WAIT_OBJECT_0 = 0
    WAIT_TIMEOUT = 258


class WinHidDevice:
    def __init__(self, handle: int, read_event: int, write_event: int, path: str):
        self.handle = handle
        self.read_event = read_event
        self.write_event = write_event
        self.path = path
        if sys.platform == "win32":
            try:
                hid_dll.HidD_SetNumInputBuffers(self.handle, 64)
                hid_dll.HidD_FlushQueue(self.handle)
            except Exception:
                pass

    def drain(self, max_reports: int = 16, timeout_ms: int = 50) -> None:
        """Drain any stale input reports from buffer."""
        if sys.platform == "win32":
            try:
                hid_dll.HidD_FlushQueue(self.handle)
            except Exception:
                pass
        while max_reports > 0:
            rep = self.read_report(timeout_ms)
            if not rep:
                break
            max_reports -= 1

    def write_report(self, data: bytes) -> bool:
        """Write a 65-byte report (0x00 + 64 bytes)."""
        if len(data) == 64:
            buf = b"\x00" + data
        elif len(data) == 65:
            buf = data
        else:
            raise ValueError(f"Invalid report size: {len(data)}")

        for attempt in range(2):
            ov = OVERLAPPED()
            ov.hEvent = self.write_event
            kernel32.ResetEvent(self.write_event)

            written = wintypes.DWORD()
            res = kernel32.WriteFile(
                self.handle,
                buf,
                len(buf),
                ctypes.byref(written),
                ctypes.byref(ov),
            )
            if not res:
                err = kernel32.GetLastError()
                if err == 997:  # ERROR_IO_PENDING
                    wait = kernel32.WaitForSingleObject(self.write_event, 5000)
                    if wait != WAIT_OBJECT_0:
                        kernel32.CancelIo(self.handle)
                        if attempt == 0:
                            time.sleep(0.05)
                            continue
                        return False
                    kernel32.GetOverlappedResult(self.handle, ctypes.byref(ov), ctypes.byref(written), False)
                elif err == 995:  # ERROR_OPERATION_ABORTED
                    if attempt == 0:
                        time.sleep(0.05)
                        continue
                    return False
                else:
                    return False
            if written.value == len(buf):
                return True
        return False

    def read_report(self, timeout_ms: int = 5000) -> bytes:
        """Read a 65-byte report, returns 64-byte payload without report ID."""
        buf = ctypes.create_string_buffer(REPORT_SIZE)
        ov = OVERLAPPED()
        ov.hEvent = self.read_event
        kernel32.ResetEvent(self.read_event)

        read = wintypes.DWORD()
        res = kernel32.ReadFile(
            self.handle,
            buf,
            REPORT_SIZE,
            ctypes.byref(read),
            ctypes.byref(ov),
        )
        if not res:
            err = kernel32.GetLastError()
            if err == 997:  # ERROR_IO_PENDING
                wait = kernel32.WaitForSingleObject(self.read_event, timeout_ms)
                if wait != WAIT_OBJECT_0:
                    kernel32.CancelIo(self.handle)
                    return b""
                kernel32.GetOverlappedResult(self.handle, ctypes.byref(ov), ctypes.byref(read), False)
            elif err == 995:  # ERROR_OPERATION_ABORTED
                return b""
            else:
                return b""

        raw = buf.raw[: read.value]
        if len(raw) >= 65 and raw[0] == 0:
            return raw[1:65]
        return raw[:64]

    def close(self) -> None:
        if self.handle and self.handle not in (INVALID_HANDLE_VALUE, -1):
            try:
                kernel32.CancelIo(self.handle)
            except Exception:
                pass
            kernel32.CloseHandle(self.handle)
            self.handle = 0
        if self.read_event and self.read_event not in (INVALID_HANDLE_VALUE, -1):
            kernel32.CloseHandle(self.read_event)
            self.read_event = 0
        if self.write_event and self.write_event not in (INVALID_HANDLE_VALUE, -1):
            kernel32.CloseHandle(self.write_event)
            self.write_event = 0



def find_windows_device(target_pid: Optional[int] = None) -> Optional[str]:
    """Find Logitech device path by VID/PID."""
    hid_guid = GUID()
    hid_dll.HidD_GetHidGuid(ctypes.byref(hid_guid))

    hdev = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(hid_guid),
        None,
        None,
        0x00000002 | 0x00000010,  # DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    )
    if hdev in (INVALID_HANDLE_VALUE, -1, None, 0):
        return None

    iface_data = SP_DEVICE_INTERFACE_DATA()
    iface_data.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
    idx = 0
    candidate_path = None

    while setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(hid_guid), idx, ctypes.byref(iface_data)):
        required = wintypes.DWORD()
        setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(iface_data), None, 0, ctypes.byref(required), None)
        buf = ctypes.create_string_buffer(required.value)
        cb_size = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
        ctypes.cast(buf, ctypes.POINTER(wintypes.DWORD))[0] = cb_size

        if setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(iface_data), buf, required.value, None, None):
            dev_path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
            lower = dev_path.lower()
            if f"vid_{HARMONY_VID:04x}" in lower:
                if target_pid:
                    if f"pid_{target_pid:04x}" in lower:
                        candidate_path = dev_path
                        break
                elif any(f"pid_{p:04x}" in lower for p in ALL_HARMONY_PIDS):
                    candidate_path = dev_path
                    break
        idx += 1

    setupapi.SetupDiDestroyDeviceInfoList(hdev)
    return candidate_path


# ==============================================================================
# Linux /dev/hidraw Transport
# ==============================================================================

class LinuxHidDevice:
    def __init__(self, fd: int, path: str):
        self.fd = fd
        self.path = path

    def write_report(self, data: bytes) -> bool:
        if len(data) == 64:
            buf = b"\x00" + data
        elif len(data) == 65:
            buf = data
        else:
            raise ValueError(f"Invalid report size: {len(data)}")
        try:
            return os.write(self.fd, buf) == len(buf)
        except OSError:
            return False

    def read_report(self, timeout_ms: int = 5000) -> bytes:
        import select
        r, _, _ = select.select([self.fd], [], [], timeout_ms / 1000.0)
        if not r:
            return b""
        try:
            data = os.read(self.fd, REPORT_SIZE)
            if len(data) >= 65 and data[0] == 0:
                return data[1:65]
            return data[:64]
        except OSError:
            return b""

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


def find_linux_device(target_pid: Optional[int] = None) -> Optional[str]:
    """Scan /sys/class/hidraw for matching Logitech vendor/product."""
    import glob
    for node in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            uevent = open(os.path.join(node, "device", "uevent"), "r").read()
            # HID_ID=0003:0000046D:0000C129
            for line in uevent.splitlines():
                if line.startswith("HID_ID="):
                    parts = line.split("=")[1].split(":")
                    vid = int(parts[1], 16)
                    pid = int(parts[2], 16)
                    if vid == HARMONY_VID:
                        if (target_pid and pid == target_pid) or (not target_pid and pid in ALL_HARMONY_PIDS):
                            return f"/dev/{os.path.basename(node)}"
        except Exception:
            continue
    return None


# ==============================================================================
# Linux usbfs Transport (/dev/bus/usb/BBB/DDD, no hidraw node)
# ==============================================================================
#
# The kernel binds no HID driver to 046D:C110-C14F (hid_ignore() in drivers/hid/hid-quirks.c, kept
# for the userspace tool concordance), so the Hub and the remotes get no /dev/hidraw* node. The
# device is then reached the way libusb does it on Linux: open its usbfs node, claim the HID
# interface, and move the 64-byte reports on its interrupt endpoints as asynchronous URBs, waited
# for with select() (control SET_REPORT when the device has no interrupt OUT endpoint). Endpoints
# come from the device's own descriptors in sysfs. The ioctl and the wait can be replaced by the
# tests (no device, no root needed).

_SYSFS_USB_DIR = "/sys/bus/usb/devices"
_DEV_USB_DIR = "/dev/bus/usb"
_usbfs_ioctl = None  # None = usbfs_ioctl() (libc); the tests put a fake device here
_usbfs_wait = None  # None = select() on the node

USB_REPORT_LEN = 64  # the reports on the wire: no report ID (the report descriptor has none)
USBFS_TIMEOUT_MS = 5000  # one write (as long as the Windows transport waits)


class UsbfsUnsupportedError(OSError):
    """This CPU architecture encodes ioctl numbers differently; the usbfs transport is not built for it."""


@dataclass(frozen=True)
class UsbHidEndpoints:
    interface: int
    ep_in: int
    in_size: int
    ep_out: Optional[int]  # None: reports go out as SET_REPORT on endpoint 0


@dataclass(frozen=True)
class LinuxUsbDevice:
    sysfs: str
    busnum: int
    devnum: int
    vid: int
    pid: int

    @property
    def node(self) -> str:
        return os.path.join(_DEV_USB_DIR, f"{self.busnum:03d}", f"{self.devnum:03d}")


def _read_sysfs(path: str) -> str:
    with open(path, "r") as f:
        return f.read().strip()


def find_linux_usb_device(target_pid: Optional[int] = None) -> Optional[LinuxUsbDevice]:
    """The Harmony device on USB, from sysfs (no driver, no hidraw node needed)."""
    try:
        names = sorted(os.listdir(_SYSFS_USB_DIR))
    except OSError:
        return None
    for name in names:
        if ":" in name:  # an interface, not a device
            continue
        d = os.path.join(_SYSFS_USB_DIR, name)
        try:
            vid = int(_read_sysfs(os.path.join(d, "idVendor")), 16)
            pid = int(_read_sysfs(os.path.join(d, "idProduct")), 16)
            if vid != HARMONY_VID:
                continue
            if (target_pid and pid != target_pid) or (not target_pid and pid not in ALL_HARMONY_PIDS):
                continue
            busnum = int(_read_sysfs(os.path.join(d, "busnum")))
            devnum = int(_read_sysfs(os.path.join(d, "devnum")))
        except (OSError, ValueError):
            continue
        return LinuxUsbDevice(d, busnum, devnum, vid, pid)
    return None


def parse_hid_endpoints(descriptors: bytes, config_value: Optional[int] = None) -> Optional[UsbHidEndpoints]:
    """The first HID interface with an interrupt IN endpoint, in the raw descriptors sysfs gives
    (device descriptor, then every configuration). config_value picks the active configuration."""
    pos = 0
    in_config = config_value is None
    iface: Optional[int] = None
    ep_in = in_size = None
    ep_out: Optional[int] = None
    found: Optional[UsbHidEndpoints] = None

    def finish() -> Optional[UsbHidEndpoints]:
        if iface is not None and ep_in is not None:
            return UsbHidEndpoints(iface, ep_in, in_size or USB_REPORT_LEN, ep_out)
        return None

    while pos + 2 <= len(descriptors):
        length, dtype = descriptors[pos], descriptors[pos + 1]
        if length < 2 or pos + length > len(descriptors):
            break
        d = descriptors[pos:pos + length]
        if dtype == 0x02 and length >= 6:  # configuration
            found = found or finish()
            if found:
                return found
            iface, ep_in, in_size, ep_out = None, None, None, None
            in_config = config_value is None or d[5] == config_value
        elif dtype == 0x04 and length >= 9 and in_config:  # interface
            found = found or finish()
            if found:
                return found
            iface, ep_in, in_size, ep_out = (d[2] if d[5] == 0x03 else None), None, None, None
        elif dtype == 0x05 and length >= 7 and in_config and iface is not None:  # endpoint
            addr, attrs = d[2], d[3]
            size = d[4] | (d[5] << 8)
            if attrs & 0x03 == 0x03:  # interrupt
                if addr & 0x80:
                    if ep_in is None:
                        ep_in, in_size = addr, size & 0x7FF
                elif ep_out is None:
                    ep_out = addr
        pos += length
    return found or finish()


def _ioc(direction: int, nr: int, size: int) -> int:
    # asm-generic/ioctl.h (x86, ARM, RISC-V, ...): dir<<30 | size<<16 | type<<8 | nr
    return (direction << 30) | (size << 16) | (ord("U") << 8) | nr


class _UsbfsCtrl(ctypes.Structure):
    _fields_ = [("bRequestType", ctypes.c_uint8), ("bRequest", ctypes.c_uint8),
                ("wValue", ctypes.c_uint16), ("wIndex", ctypes.c_uint16),
                ("wLength", ctypes.c_uint16), ("timeout", ctypes.c_uint32),
                ("data", ctypes.c_void_p)]


class _UsbfsUrb(ctypes.Structure):
    # struct usbdevfs_urb without the iso_frame_desc[] tail (not used for interrupt transfers)
    _fields_ = [("type", ctypes.c_uint8), ("endpoint", ctypes.c_uint8), ("status", ctypes.c_int),
                ("flags", ctypes.c_uint), ("buffer", ctypes.c_void_p), ("buffer_length", ctypes.c_int),
                ("actual_length", ctypes.c_int), ("start_frame", ctypes.c_int),
                ("number_of_packets", ctypes.c_int), ("error_count", ctypes.c_int),
                ("signr", ctypes.c_uint), ("usercontext", ctypes.c_void_p)]


# linux/usbdevice_fs.h
USBDEVFS_URB_TYPE_INTERRUPT = 1
USBDEVFS_CONTROL = _ioc(3, 0, ctypes.sizeof(_UsbfsCtrl))
USBDEVFS_SUBMITURB = _ioc(2, 10, ctypes.sizeof(_UsbfsUrb))
USBDEVFS_DISCARDURB = _ioc(0, 11, 0)
USBDEVFS_REAPURB = _ioc(1, 12, ctypes.sizeof(ctypes.c_void_p))
USBDEVFS_REAPURBNDELAY = _ioc(1, 13, ctypes.sizeof(ctypes.c_void_p))
USBDEVFS_CLAIMINTERFACE = _ioc(2, 15, ctypes.sizeof(ctypes.c_uint))
USBDEVFS_RELEASEINTERFACE = _ioc(2, 16, ctypes.sizeof(ctypes.c_uint))

# errno values that mean the device is no longer on the bus: every later request fails the same way
_USB_GONE_ERRNOS = (errno.ENODEV, errno.ESHUTDOWN)

_libc_ioctl_fn = None


def usbfs_ioctl(fd: int, request: int, arg=None) -> int:
    """ioctl(fd, request, &arg) through libc, with the address of arg itself (a ctypes object).
    fcntl.ioctl() would pass a temporary copy, but the kernel keeps the address of a submitted URB
    and writes its status there when it is reaped. Raises OSError on -1."""
    global _libc_ioctl_fn
    if _libc_ioctl_fn is None:
        fn = ctypes.CDLL(None, use_errno=True).ioctl
        fn.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p]
        fn.restype = ctypes.c_int
        _libc_ioctl_fn = fn
    r = _libc_ioctl_fn(fd, request, None if arg is None else ctypes.addressof(arg))
    if r == -1:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))
    return r


def _select_writable(fd: int, timeout_s: float) -> bool:
    """usbfs reports a completed URB as "writable" (usbdev_poll: EPOLLOUT); a select() that Ctrl-C
    interrupts, unlike a synchronous usbfs transfer."""
    import select
    _, w, _ = select.select([], [fd], [], max(0.0, timeout_s))
    return bool(w)


def _usbfs_arch_supported() -> bool:
    m = platform.machine().lower()
    return not m.startswith(("mips", "ppc", "powerpc", "sparc", "alpha", "parisc"))


class UsbfsHidDevice:
    """A Harmony HID device on its usbfs node: 64-byte reports on the interrupt endpoints, each one
    an asynchronous URB as libusb submits them (one in flight at a time)."""

    def __init__(self, fd: int, path: str, endpoints: UsbHidEndpoints, ioctl=None, wait=None):
        self.fd = fd
        self.path = path
        self.endpoints = endpoints
        self._ioctl = ioctl or usbfs_ioctl
        self._wait = wait or _select_writable
        self._claimed = False
        self.gone = False  # the device left the bus: nothing more will come
        self.last_error: Optional[OSError] = None  # the first transfer error, for diagnosis
        self._warned = False

    def claim(self) -> None:
        self._ioctl(self.fd, USBDEVFS_CLAIMINTERFACE, ctypes.c_uint(self.endpoints.interface))
        self._claimed = True

    def _reap(self, block: bool) -> bool:
        out = ctypes.c_void_p()
        try:
            self._ioctl(self.fd, USBDEVFS_REAPURB if block else USBDEVFS_REAPURBNDELAY, out)
        except OSError as e:
            if e.errno == errno.EAGAIN:
                return False
            raise
        return True

    def _transfer(self, ep: int, buf, length: int, timeout_ms: int) -> int:
        """One interrupt transfer. A URB still running at the deadline (or at Ctrl-C) is cancelled
        and then collected, so a report that completes during the cancel is kept, not lost.
        Returns the bytes moved; raises TimeoutError, or OSError with the URB's status."""
        urb = _UsbfsUrb(type=USBDEVFS_URB_TYPE_INTERRUPT, endpoint=ep, status=-errno.EINPROGRESS,
                        buffer=ctypes.addressof(buf), buffer_length=length)
        self._ioctl(self.fd, USBDEVFS_SUBMITURB, urb)
        reaped = False
        reap_error: Optional[OSError] = None
        try:
            deadline = time.monotonic() + max(1, int(timeout_ms)) / 1000.0
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not self._wait(self.fd, remaining):
                    break
                if self._reap(block=False):
                    reaped = True
                    break
                time.sleep(0.001)  # ready without a completed URB: do not spin
        except OSError as e:
            reap_error = e
        finally:
            if not reaped:
                try:
                    self._ioctl(self.fd, USBDEVFS_DISCARDURB, urb)
                except OSError:
                    pass  # it completed in the meantime (EINVAL): collected below all the same
                try:
                    self._reap(block=True)
                except OSError as e:
                    reap_error = reap_error or e
        if reap_error is not None:
            raise OSError(reap_error.errno, reap_error.strerror, self.path)
        if urb.status == 0:
            return urb.actual_length
        if not reaped and urb.status in (-errno.ENOENT, -errno.ECONNRESET):  # cancelled at the deadline
            raise TimeoutError(errno.ETIMEDOUT, "no report", self.path)
        raise OSError(-urb.status, os.strerror(-urb.status), self.path)

    def _note_error(self, e: OSError) -> None:
        if e.errno in _USB_GONE_ERRNOS:
            self.gone = True
        if self.last_error is None:
            self.last_error = e
        if not self._warned:
            self._warned = True
            what = "left USB" if self.gone else f"USB transfer error: {e.strerror or e}"
            print(f"[!] The Harmony device {what} ({self.path}).", file=sys.stderr)

    def write_report(self, data: bytes) -> bool:
        if len(data) == 65:
            data = data[1:]  # the report ID byte of the hidraw/Windows framing; not on the wire
        elif len(data) != 64:
            raise ValueError(f"Invalid report size: {len(data)}")
        if self.gone:
            return False
        buf = ctypes.create_string_buffer(bytes(data), USB_REPORT_LEN)
        try:
            if self.endpoints.ep_out is not None:
                n = self._transfer(self.endpoints.ep_out, buf, USB_REPORT_LEN, USBFS_TIMEOUT_MS)
            else:
                req = _UsbfsCtrl(0x21, 0x09, 0x0200, self.endpoints.interface,  # SET_REPORT, Output, ID 0
                                 USB_REPORT_LEN, USBFS_TIMEOUT_MS, ctypes.addressof(buf))
                n = int(self._ioctl(self.fd, USBDEVFS_CONTROL, req))
        except OSError as e:  # TimeoutError included
            self._note_error(e)
            return False
        return n == USB_REPORT_LEN

    def read_report(self, timeout_ms: int = 5000) -> bytes:
        """64-byte report, or b"" when none came in time. Other errors are kept in last_error
        (gone=True when the device left the bus, so the callers stop waiting)."""
        if self.gone:
            return b""
        buf = ctypes.create_string_buffer(max(self.endpoints.in_size, USB_REPORT_LEN))
        try:
            n = self._transfer(self.endpoints.ep_in, buf, len(buf), timeout_ms)
        except TimeoutError:
            return b""
        except OSError as e:
            self._note_error(e)
            if not self.gone:
                time.sleep(0.01)  # an endpoint error comes back at once: no busy loop
            return b""
        return buf.raw[:min(n, USB_REPORT_LEN)]

    def close(self) -> None:
        if self.fd >= 0:
            if self._claimed:
                try:
                    self._ioctl(self.fd, USBDEVFS_RELEASEINTERFACE, ctypes.c_uint(self.endpoints.interface))
                except OSError:
                    pass
                self._claimed = False
            os.close(self.fd)  # the kernel also cancels any URB left
            self.fd = -1


def open_usbfs_device(dev: LinuxUsbDevice) -> UsbfsHidDevice:
    """Open and claim the device. Raises OSError (with the node as filename): PermissionError when
    this user may not open the node, errno EBUSY when someone else holds the interface,
    UsbfsUnsupportedError, ENODEV."""
    if _usbfs_ioctl is None and not _usbfs_arch_supported():
        raise UsbfsUnsupportedError(errno.ENOTSUP, f"usbfs transport not built for {platform.machine()}", dev.node)
    try:
        with open(os.path.join(dev.sysfs, "descriptors"), "rb") as f:
            raw = f.read()
    except OSError as e:
        raise OSError(errno.ENODEV, f"cannot read the USB descriptors ({e.strerror})", dev.sysfs) from e
    config: Optional[int] = None
    try:
        config = int(_read_sysfs(os.path.join(dev.sysfs, "bConfigurationValue")))
    except (OSError, ValueError):
        pass
    endpoints = parse_hid_endpoints(raw, config)
    if endpoints is None:
        raise OSError(errno.ENODEV, "no HID interface with an interrupt IN endpoint", dev.sysfs)
    fd = os.open(dev.node, os.O_RDWR)
    hid = UsbfsHidDevice(fd, dev.node, endpoints, _usbfs_ioctl, _usbfs_wait)
    try:
        hid.claim()
    except OSError as e:
        hid.close()
        raise OSError(e.errno, e.strerror, dev.node) from e
    return hid


# udev rules for the Harmony devices (same text in docs/FIRST_INSTALL_HUB.md). The first gives the
# user logged in at the desktop access (uaccess). The second is for logins without a desktop on
# Debian and Ubuntu: newer systemd drops a whole rule line that names a group the system lacks, so
# GROUP="plugdev" is kept out of the first one.
LINUX_UDEV_RULE = 'SUBSYSTEM=="usb", ATTR{idVendor}=="046d", ATTR{idProduct}=="c129|c12b|c132", TAG+="uaccess"'
LINUX_UDEV_RULE_PLUGDEV = ('SUBSYSTEM=="usb", ATTR{idVendor}=="046d", ATTR{idProduct}=="c129|c12b|c132", '
                           'MODE="0660", GROUP="plugdev"')


def usb_open_failure_message(candidates) -> Optional[str]:
    """Why a Harmony device that is on USB could not be opened (from the open_error of the
    HarmonyHid objects tried), with what to do. None when none was found at all: the caller then
    prints its own "not detected" text."""
    err: Optional[OSError] = None
    for c in candidates:
        e = getattr(c, "open_error", None)
        if e is not None and (err is None or isinstance(e, PermissionError)):
            err = e
    if err is None:
        return None
    where = getattr(err, "filename", None) or "the device"
    if isinstance(err, PermissionError) and str(where).startswith("/dev/hidraw"):
        # a kernel that made a hidraw node (no hid_ignore() for the device, or the usbhid quirk)
        lines = [f"The Harmony device is on USB, but this user may not open its device ({where}).",
                 "Run the tool with sudo, or give your user read and write access to that hidraw node."]
    elif isinstance(err, PermissionError):
        lines = [f"The Harmony device is on USB, but this user may not open its device ({where}).",
                 "Run the tool with sudo, or add this udev rule as /etc/udev/rules.d/60-harmony.rules",
                 "(access for the user logged in at the desktop):",
                 f"  {LINUX_UDEV_RULE}",
                 "Without a desktop login, on Debian or Ubuntu, use this line instead and add your user",
                 "to plugdev ('sudo usermod -aG plugdev $USER', then log in again):",
                 f"  {LINUX_UDEV_RULE_PLUGDEV}",
                 "Then run 'sudo udevadm control --reload' and connect the device again."]
    elif err.errno == errno.EBUSY:
        lines = [f"The Harmony device is on USB, but it is in use ({where}).",
                 "Another program, a virtual machine or a kernel driver holds it: close other Harmony",
                 "tools (or detach the device from the virtual machine), then try again."]
    else:
        lines = [f"The Harmony device is on USB, but it could not be opened: {err.strerror or err} ({where})."]
    return "\n".join(lines)


# ==============================================================================
# macOS IOKit Transport (ctypes)
# ==============================================================================

if sys.platform == "darwin":
    import ctypes.util
    import queue
    import threading

    def _load_framework(name: str):
        path = f"/System/Library/Frameworks/{name}.framework/{name}"
        try:
            return ctypes.cdll.LoadLibrary(path)
        except OSError:
            lib_name = ctypes.util.find_library(name)
            if lib_name:
                return ctypes.cdll.LoadLibrary(lib_name)
            raise

    iokit = _load_framework("IOKit")
    cf = _load_framework("CoreFoundation")

    # CoreFoundation prototypes
    cf.CFStringCreateWithCString.restype = ctypes.c_void_p
    cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
    cf.CFNumberGetValue.restype = ctypes.c_bool
    cf.CFNumberGetValue.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_void_p]
    cf.CFSetGetCount.restype = ctypes.c_long
    cf.CFSetGetCount.argtypes = [ctypes.c_void_p]
    cf.CFSetGetValues.restype = None
    cf.CFSetGetValues.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    cf.CFRetain.restype = ctypes.c_void_p
    cf.CFRetain.argtypes = [ctypes.c_void_p]
    cf.CFRelease.restype = None
    cf.CFRelease.argtypes = [ctypes.c_void_p]
    cf.CFRunLoopGetCurrent.restype = ctypes.c_void_p
    cf.CFRunLoopGetCurrent.argtypes = []
    cf.CFRunLoopStop.restype = None
    cf.CFRunLoopStop.argtypes = [ctypes.c_void_p]
    cf.CFRunLoopRunInMode.restype = ctypes.c_int32
    cf.CFRunLoopRunInMode.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_bool]

    def _cf_string(s: str) -> int:
        return cf.CFStringCreateWithCString(None, s.encode("utf-8"), 0x08000100)

    try:
        kCFRunLoopDefaultMode = ctypes.c_void_p.in_dll(cf, "kCFRunLoopDefaultMode").value
    except Exception:
        kCFRunLoopDefaultMode = _cf_string("kCFRunLoopDefaultMode")

    CF_STR_VENDOR_ID = _cf_string("VendorID")
    CF_STR_PRODUCT_ID = _cf_string("ProductID")

    def _get_cf_int(val_ref) -> Optional[int]:
        if not val_ref:
            return None
        val = ctypes.c_int32()
        if cf.CFNumberGetValue(val_ref, 3, ctypes.byref(val)):
            return val.value
        return None

    # IOKit prototypes
    iokit.IOHIDManagerCreate.restype = ctypes.c_void_p
    iokit.IOHIDManagerCreate.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    iokit.IOHIDManagerSetDeviceMatching.restype = None
    iokit.IOHIDManagerSetDeviceMatching.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    iokit.IOHIDManagerCopyDevices.restype = ctypes.c_void_p
    iokit.IOHIDManagerCopyDevices.argtypes = [ctypes.c_void_p]

    iokit.IOHIDDeviceOpen.restype = ctypes.c_int32
    iokit.IOHIDDeviceOpen.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    iokit.IOHIDDeviceClose.restype = ctypes.c_int32
    iokit.IOHIDDeviceClose.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    iokit.IOHIDDeviceGetProperty.restype = ctypes.c_void_p
    iokit.IOHIDDeviceGetProperty.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    iokit.IOHIDDeviceSetReport.restype = ctypes.c_int32
    iokit.IOHIDDeviceSetReport.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_long,
        ctypes.c_char_p,
        ctypes.c_long,
    ]

    IO_HID_REPORT_CALLBACK = ctypes.CFUNCTYPE(
        None,
        ctypes.c_void_p,
        ctypes.c_int32,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_long,
    )

    iokit.IOHIDDeviceRegisterInputReportCallback.restype = None
    iokit.IOHIDDeviceRegisterInputReportCallback.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_long,
        IO_HID_REPORT_CALLBACK,
        ctypes.c_void_p,
    ]
    iokit.IOHIDDeviceScheduleWithRunLoop.restype = None
    iokit.IOHIDDeviceScheduleWithRunLoop.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    iokit.IOHIDDeviceUnscheduleFromRunLoop.restype = None
    iokit.IOHIDDeviceUnscheduleFromRunLoop.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]


class MacHidDevice:
    def __init__(self, manager: int, dev_ref: int):
        self.manager = manager
        self.dev_ref = dev_ref
        import queue
        import threading
        self.queue: queue.Queue[bytes] = queue.Queue()
        self.run_loop = None
        self.running = False
        self.thread: Optional[threading.Thread] = None
        self.ready_event = threading.Event()
        self.report_buf = (ctypes.c_uint8 * 256)()

        def _cb(context, result, sender, report_type, report_id, report_ptr, report_length):
            if result != 0 or not report_ptr or report_length <= 0:
                return
            data = ctypes.string_at(report_ptr, report_length)
            if len(data) >= 65 and data[0] == 0:
                clean = data[1:65]
            else:
                clean = data[:64]
            self.queue.put(clean)

        self._c_callback = IO_HID_REPORT_CALLBACK(_cb)
        iokit.IOHIDDeviceRegisterInputReportCallback(
            self.dev_ref,
            self.report_buf,
            len(self.report_buf),
            self._c_callback,
            None,
        )

        self.running = True
        self.thread = threading.Thread(target=self._run_loop_worker, daemon=True)
        self.thread.start()
        self.ready_event.wait(timeout=2.0)

    def _run_loop_worker(self):
        self.run_loop = cf.CFRunLoopGetCurrent()
        cf.CFRetain(self.run_loop)
        iokit.IOHIDDeviceScheduleWithRunLoop(
            self.dev_ref,
            self.run_loop,
            kCFRunLoopDefaultMode,
        )
        self.ready_event.set()
        while self.running:
            cf.CFRunLoopRunInMode(kCFRunLoopDefaultMode, ctypes.c_double(0.1), True)

        try:
            iokit.IOHIDDeviceUnscheduleFromRunLoop(
                self.dev_ref,
                self.run_loop,
                kCFRunLoopDefaultMode,
            )
        except Exception:
            pass
        if self.run_loop:
            cf.CFRelease(self.run_loop)
            self.run_loop = None

    def drain(self, max_reports: int = 16, timeout_ms: int = 50) -> None:
        """Drain any stale input reports from buffer."""
        while not self.queue.empty():
            try:
                self.queue.get_nowait()
            except Exception:
                break
        while max_reports > 0:
            rep = self.read_report(timeout_ms)
            if not rep:
                break
            max_reports -= 1

    def write_report(self, data: bytes) -> bool:
        if len(data) == 64:
            report_id = 0
            payload = data
        elif len(data) == 65:
            report_id = data[0]
            payload = data[1:] if report_id == 0 else data
        else:
            raise ValueError(f"Invalid report size: {len(data)}")

        res = iokit.IOHIDDeviceSetReport(
            self.dev_ref,
            1,  # kIOHIDReportTypeOutput
            report_id,
            payload,
            len(payload),
        )
        if res != 0:
            res = iokit.IOHIDDeviceSetReport(
                self.dev_ref,
                2,  # kIOHIDReportTypeFeature fallback
                report_id,
                payload,
                len(payload),
            )
        return res == 0

    def read_report(self, timeout_ms: int = 5000) -> bytes:
        timeout_sec = max(0.001, timeout_ms / 1000.0)
        try:
            return self.queue.get(timeout=timeout_sec)
        except Exception:
            return b""

    def close(self) -> None:
        self.running = False
        if self.run_loop:
            try:
                cf.CFRunLoopStop(self.run_loop)
            except Exception:
                pass
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout=1.0)
            self.thread = None

        if self.dev_ref:
            try:
                iokit.IOHIDDeviceRegisterInputReportCallback(
                    self.dev_ref,
                    self.report_buf,
                    len(self.report_buf),
                    IO_HID_REPORT_CALLBACK(),
                    None,
                )
            except Exception:
                pass
            try:
                iokit.IOHIDDeviceClose(self.dev_ref, 0)
            except Exception:
                pass
            try:
                cf.CFRelease(self.dev_ref)
            except Exception:
                pass
            self.dev_ref = None

        if self.manager:
            try:
                cf.CFRelease(self.manager)
            except Exception:
                pass
            self.manager = None


def find_darwin_device(target_pid: Optional[int] = None) -> tuple[Optional[int], Optional[int]]:
    """Scan macOS IOHIDManager for matching Logitech vendor/product."""
    mgr = iokit.IOHIDManagerCreate(None, 0)
    if not mgr:
        return None, None
    iokit.IOHIDManagerSetDeviceMatching(mgr, None)
    # Do not call IOHIDManagerOpen: it opens every matched HID device and fails if any one
    # refuses (e.g. Magic Keyboard with Touch ID returns kIOReturnUnsupported). Enumeration
    # works without it, and open() calls IOHIDDeviceOpen on the matched device only.
    dev_set = iokit.IOHIDManagerCopyDevices(mgr)
    matched_device = None
    if dev_set:
        count = cf.CFSetGetCount(dev_set)
        if count > 0:
            dev_array = (ctypes.c_void_p * count)()
            cf.CFSetGetValues(dev_set, dev_array)
            for d in dev_array:
                if not d:
                    continue
                vid_ref = iokit.IOHIDDeviceGetProperty(d, CF_STR_VENDOR_ID)
                pid_ref = iokit.IOHIDDeviceGetProperty(d, CF_STR_PRODUCT_ID)
                vid = _get_cf_int(vid_ref)
                pid = _get_cf_int(pid_ref)
                if vid == HARMONY_VID:
                    if target_pid:
                        if pid == target_pid:
                            matched_device = d
                            break
                    elif pid in ALL_HARMONY_PIDS:
                        matched_device = d
                        break
        cf.CFRelease(dev_set)

    if matched_device:
        cf.CFRetain(matched_device)
        return mgr, matched_device

    cf.CFRelease(mgr)
    return None, None


# ==============================================================================
# High-Level Harmony HID Bridge
# ==============================================================================

class HarmonyHid:
    def __init__(self, pid: Optional[int] = None):
        self.pid = pid
        self.dev = None
        self.seq = 1
        # why the last open() failed when a device was found but could not be opened (Linux:
        # the OSError of the hidraw or usbfs node, e.g. PermissionError); None otherwise.
        # usb_open_failure_message() turns it into text for the user.
        self.open_error: Optional[OSError] = None

    def open(self) -> bool:
        if sys.platform == "win32":
            path = find_windows_device(self.pid)
            if not path:
                return False
            h = kernel32.CreateFileW(
                path,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                None,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                None,
            )
            if h in (INVALID_HANDLE_VALUE, -1, None, 0):
                return False
            ev_r = kernel32.CreateEventW(None, True, False, None)
            ev_w = kernel32.CreateEventW(None, True, False, None)
            self.dev = WinHidDevice(h, ev_r, ev_w, path)
            self.drain_input()
            return True

        elif sys.platform.startswith("linux"):
            self.open_error = None
            # a hidraw node exists only where the kernel does not ignore the device (usbhid quirk
            # HID_QUIRK_NO_IGNORE, or a kernel without the Harmony range in hid_ignore())
            path = find_linux_device(self.pid)
            if path:
                try:
                    fd = os.open(path, os.O_RDWR)
                    self.dev = LinuxHidDevice(fd, path)
                    return True
                except OSError as e:
                    self.open_error = e
                    return False
            usb = find_linux_usb_device(self.pid)
            if not usb:
                return False
            try:
                self.dev = open_usbfs_device(usb)
            except OSError as e:
                self.open_error = e
                return False
            self.drain_input()
            return True
        elif sys.platform == "darwin":
            mgr, dev_ref = find_darwin_device(self.pid)
            if not dev_ref:
                return False
            ret = iokit.IOHIDDeviceOpen(dev_ref, 0)
            if ret != 0:
                if mgr:
                    cf.CFRelease(mgr)
                cf.CFRelease(dev_ref)
                return False
            self.dev = MacHidDevice(mgr, dev_ref)
            return True
        else:
            raise NotImplementedError(f"Unsupported OS: {sys.platform}")

    def close(self) -> None:
        if self.dev:
            self.dev.close()
            self.dev = None

    def __enter__(self):
        if not self.open():
            raise RuntimeError("Could not connect to Logitech Harmony USB HID device.")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _next_seq(self) -> int:
        """1..126: the answer to sequence 127 would carry 0xFF in byte 2, the error marker"""
        s = self.seq
        self.seq = self.seq + 1
        if self.seq > 126:
            self.seq = 1
        return s

    def _device_gone(self) -> bool:
        """The device left the bus (usbfs transport): no answer will come, stop waiting."""
        return bool(getattr(self.dev, "gone", False))

    def drain_input(self, timeout_ms: int = 10) -> None:
        """Discard any stale/unread reports in the HID endpoint buffer."""
        if sys.platform == "win32" and self.dev and getattr(self.dev, "handle", None):
            try:
                hid_dll.HidD_FlushQueue(self.dev.handle)
            except Exception:
                pass
        while self.dev:
            r = self.dev.read_report(timeout_ms=timeout_ms)
            if not r:
                break

    def command(self, cmd_id: int, params: list[bytes], timeout_ms: int = 5000) -> bytes:
        """Send primary command frame and wait for matching response."""
        self.drain_input(timeout_ms=10)
        seq = self._next_seq()
        frames = build_primary_frames(cmd_id, seq, params)
        for f in frames:
            if not self.dev.write_report(f):
                raise IOError("HID report write failed")

        t_end = time.time() + (timeout_ms / 1000.0)
        while time.time() < t_end:
            resp = self.dev.read_report(int((t_end - time.time()) * 1000))
            if not resp:
                if self._device_gone():
                    break
                continue
            for off in range(0, max(0, len(resp) - 3)):
                if resp[off] == 0xFF and resp[off + 1] == cmd_id:
                    return resp[off:]
                if resp[off] == 0xFF and resp[off + 1] == 0xFF:
                    return resp[off:]
        return b""

    def open_file_write(self, remote_path: str, size: Optional[int] = None, timeout_ms: int = 15000) -> int:
        """Open a file for writing via command 0x01. Returns handle ID."""
        params = [encode_param_string(remote_path), encode_param_string("W")]
        if size is not None:
            params.append(encode_param_dword_be(size))

        resp = self.command(0x01, params, timeout_ms)
        if not resp or resp[0] != 0xFF or resp[1] != 0x01:
            raise RuntimeError(f"Open file '{remote_path}' failed: response {resp.hex() if resp else 'timeout'}")
        if is_error_response(resp):
            raise _refused(f"Open file '{remote_path}' for writing", resp)

        # Byte 5 carries the 1-byte handle ID
        # Format: FF 01 <seq> <params> 01 <handle_id> ...
        if len(resp) >= 6 and resp[4] == 0x01:
            return resp[5]
        return resp[2]  # fallback handle byte

    def close_file(self, handle_id: int, timeout_ms: int = 10000) -> bool:
        """Close an open file handle via command 0x07."""
        params = [encode_param_byte(handle_id)]
        resp = self.command(0x07, params, timeout_ms)
        return bool(resp and resp[0] == 0xFF and resp[1] == 0x07 and not is_error_response(resp))

    def write_file_chunks(
        self,
        handle_id: int,
        data: bytes,
        packets_per_chunk: int = 5,
        timeout_ms: int = 15000,
        progress_callback=None,
        count_width: str = "word",
    ) -> bool:
        """Stream data in chunks of data frames (cmd 0x03)."""
        if count_width not in ("byte", "word"):
            raise ValueError(f"Unsupported count_width: {count_width!r}")
        data_frames = build_data_frames(data, chunk_size=62)
        total_data_frames = len(data_frames)
        sent_frames = 0

        while sent_frames < total_data_frames:
            burst = data_frames[sent_frames : sent_frames + packets_per_chunk]
            seq = self._next_seq()

            # Command 0x03 parameters: [handle_id, total_reports_in_burst]
            total_reports = 1 + len(burst)
            if count_width == "byte":
                if total_reports > 0xFF:
                    raise ValueError(f"Burst report count {total_reports} exceeds byte count_width")
                count_param = encode_param_byte(total_reports)
            else:
                count_param = encode_param_word_be(total_reports)
            params = [encode_param_byte(handle_id), count_param]
            cmd_frame = build_primary_frames(0x03, seq, params)[0]

            if not self.dev.write_report(cmd_frame):
                return False

            for df in burst:
                if not self.dev.write_report(df):
                    return False

            # Wait for cmd 0x03 ACK
            t_end = time.time() + (timeout_ms / 1000.0)
            acked = False
            while time.time() < t_end:
                resp = self.dev.read_report(int((t_end - time.time()) * 1000))
                if not resp:
                    if self._device_gone():
                        break
                    continue
                # a refusal is 6 bytes (is_error_response): only offsets with a whole one after them
                for off in range(0, max(0, len(resp) - 5)):
                    if resp[off] == 0xFF and resp[off + 1] == 0x03:
                        if is_error_response(resp[off:]):
                            raise _refused(f"Write at offset {sent_frames * 62}/{len(data)}", resp[off:])
                        acked = True
                        break
                if acked:
                    break
            if not acked:
                raise RuntimeError(f"Write chunk timeout at offset {sent_frames * 62}/{len(data)}")

            sent_frames += len(burst)
            if progress_callback:
                progress_callback(min(sent_frames * 62, len(data)), len(data))

        return True

    def write_full_file(
        self,
        remote_path: str,
        data: bytes,
        packets_per_chunk: int = 5,
        progress_callback=None,
    ) -> bool:
        """Complete workflow: open -> write chunks -> close. False when a report write failed:
        the file on the device is then cut short."""
        handle_id = self.open_file_write(remote_path, size=len(data))
        written = False
        try:
            written = self.write_file_chunks(handle_id, data, packets_per_chunk, progress_callback=progress_callback)
        finally:
            self.close_file(handle_id)
        return written

    def write_simple(self, remote_path: str, payload: bytes) -> bool:
        """Write a small single-burst endpoint (e.g. /tde/enable or /sys/reboot). False when the
        data report was not written."""
        handle_id = self.open_file_write(remote_path, size=len(payload))
        written = False
        try:
            burst_pkts = max(1, (len(payload) + 61) // 62)
            written = self.write_file_chunks(handle_id, payload, packets_per_chunk=burst_pkts) if payload else True
        finally:
            self.close_file(handle_id)
        return written

    def open_file_read(self, remote_path: str, timeout_ms: int = 15000) -> tuple[int, int]:
        """Open a file for reading via command 0x01 ('R'). Returns (handle_id, file_size)."""
        params = [encode_param_string(remote_path), encode_param_string("R")]
        resp = self.command(0x01, params, timeout_ms)
        if not resp or resp[0] != 0xFF or resp[1] != 0x01:
            raise RuntimeError(f"Open read '{remote_path}' failed: response {resp.hex() if resp else 'timeout'}")
        if is_error_response(resp):
            raise _refused(f"Open file '{remote_path}' for reading", resp)

        handle_id = resp[5] if len(resp) >= 6 else resp[2]
        file_size = int.from_bytes(resp[7:11], "big") if len(resp) >= 11 else 0
        return handle_id, file_size

    def read_file_chunks(
        self,
        handle_id: int,
        size: int,
        packets_per_read: int = 50,
        timeout_ms: int = 15000,
        progress_callback=None,
    ) -> bytes:
        """Read data chunks via command 0x04."""
        data = bytearray()
        while len(data) < size:
            wanted = min(size - len(data), packets_per_read * 62)
            seq = self._next_seq()
            params = [encode_param_byte(handle_id), encode_param_byte(packets_per_read)]
            cmd_frame = build_primary_frames(0x04, seq, params)[0]
            if not self.dev.write_report(cmd_frame):
                break

            chunk_received = bytearray()
            t_end = time.time() + (timeout_ms / 1000.0)
            while time.time() < t_end and len(chunk_received) < wanted:
                rep = self.dev.read_report(int(max(10, (t_end - time.time()) * 1000)))
                if not rep:
                    if self._device_gone():
                        break
                    continue
                if rep[0] == 0xFF:
                    continue
                if rep[0] == 0xFE:
                    break
                pkt_len = rep[1] & 0x3F
                if pkt_len == 0:
                    pkt_len = min(62, wanted - len(chunk_received))
                take = min(pkt_len, wanted - len(chunk_received))
                chunk_received.extend(rep[2 : 2 + take])

            if not chunk_received:
                break
            data.extend(chunk_received)
            if progress_callback:
                progress_callback(len(data), size)

        return bytes(data)

    def read_file(self, remote_path: str, packets_per_read: int = 50, timeout_ms: int = 15000) -> bytes:
        """Complete workflow: open read -> read chunks -> close."""
        handle_id, size = self.open_file_read(remote_path, timeout_ms)
        try:
            return self.read_file_chunks(handle_id, size, packets_per_read, timeout_ms)
        finally:
            self.close_file(handle_id)

    # --------------------------------------------------------------------------
    # Specialized Hub / Remote Actions
    # --------------------------------------------------------------------------

    def set_tde_enable(self, state: bool = True) -> bool:
        """Set /etc/tdeenable on device via /tde/enable endpoint."""
        val = b"1" if state else b"0"
        try:
            return self.write_simple("/tde/enable", val)
        except Exception:
            return False

    def reboot(self) -> bool:
        """Send reboot signal via /sys/reboot."""
        try:
            return self.write_simple("/sys/reboot", b"reboot")
        except Exception:
            return True  # Device frequently resets USB bus immediately

    def provision_wifi(self, ssid: str, password: str, encryption: str = "WPA2-PSK") -> bool:
        """Provision Wi-Fi directly over USB HID in a single byte-counted burst. Raises
        HidErrorResponse when the Hub refuses the request; the result of the attempt itself is
        in get_wifi_status() afterwards (harmony-ng: connect_status "failed" and error_code)."""
        for field_name, val in (("ssid", ssid), ("password", password), ("encryption", encryption)):
            if any(ch in val for ch in ("\r", "\n", "\x00")):
                raise ValueError(f"Wi-Fi {field_name} must not contain CR, LF, or NUL characters")
        payload = f"ssid,{ssid}\npassword,{password}\nencryption,{encryption}\n".encode("utf-8")
        handle_id = self.open_file_write("/sys/wifi/connect", size=len(payload), timeout_ms=60000)
        written = False
        try:
            written = self.write_file_chunks(
                handle_id,
                payload,
                packets_per_chunk=50,
                timeout_ms=100000,
                count_width="byte",
            )
        finally:
            try:
                self.close_file(handle_id, timeout_ms=30000)
            except Exception:
                pass
        return written

    def get_wifi_status(self) -> dict[str, str]:
        """Query Wi-Fi status over USB HID, returning parsed key-value dict."""
        try:
            raw = self.read_file("/sys/wifi/connect")
            text = raw.decode("utf-8", errors="replace")
            info: dict[str, str] = {}
            for line in text.splitlines():
                if "," in line:
                    k, v = line.split(",", 1)
                    info[k.strip()] = v.strip()
            return info
        except Exception:
            return {}

    def get_sysinfo(self) -> dict[str, str]:
        """Query /sys/sysinfo or /rf/deviceinfo returning parsed properties."""
        for path in ("/sys/sysinfo", "/rf/deviceinfo"):
            try:
                raw = self.read_file(path)
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace")
                info: dict[str, str] = {}
                tokens = []
                for chunk in text.split("\x00"):
                    for line in chunk.splitlines():
                        line = line.strip()
                        if line:
                            tokens.append(line)
                for item in tokens:
                    if "," in item:
                        k, v = item.split(",", 1)
                    elif "=" in item:
                        k, v = item.split("=", 1)
                    elif " " in item:
                        k, v = item.split(" ", 1)
                    else:
                        continue
                    info[k.strip()] = v.strip()
                if info:
                    if "fw_ver" in info and "sw_version" not in info:
                        info["sw_version"] = info["fw_ver"]
                    elif "sw_version" in info and "fw_ver" not in info:
                        info["fw_ver"] = info["sw_version"]
                    return info
            except Exception:
                continue
        return {}


    def reset_filesystem(self, timeout_ms: int = 5000) -> bool:
        """Reset USB file system state on device via cmd 0xFF 0x66."""
        resp = self.command(0xFF, [encode_param_byte(0x66)], timeout_ms=timeout_ms)
        return bool(resp and resp[0] == 0xFF and resp[1] == 0xFF and not is_error_response(resp))

    def finalize_reset(self, timeout_ms: int = 5000) -> bool:
        """Trigger device reboot into bootloader flasher via cmd 0xFF 0x00."""
        try:
            self.command(0xFF, [encode_param_byte(0x00)], timeout_ms=timeout_ms)
            return True
        except Exception:
            return True  # Device frequently disconnects USB immediately on reboot

    def verify_checksum(
        self,
        handle_id: int,
        expected_md5: str,
        length: int,
        offset: int = 0,
        seed: int = 0,
        checksum_type: str = "MD5",
        timeout_ms: int = 30000,
    ) -> tuple[bool, str]:
        """Verify firmware checksum on device handle via cmd 0x06."""
        params = [
            encode_param_byte(handle_id),
            encode_param_byte(0x01),
            encode_param_string(checksum_type),
            encode_param_word_be(seed),
            encode_param_dword_be(offset),
            encode_param_dword_be(length),
            encode_param_string(expected_md5),
        ]
        resp = self.command(0x06, params, timeout_ms=timeout_ms)
        if not resp:
            return False, "timeout"
        if b"m\x00" in resp or ord("m") in resp[4:10]:
            return True, "match"
        if b"u\x00" in resp or ord("u") in resp[4:10]:
            return True, "bootloader_handoff"
        return False, f"status_{resp.hex()}"
