#!/usr/bin/env python3
"""Clean-room Logitech Harmony USB HID Client.

Communicates with Logitech Harmony Hub (046D:C129) and Elite Remote (046D:C12B)
over raw USB HID without third-party dependencies.

Supported OS:
- Windows: Native ctypes (setupapi.dll + hid.dll + kernel32.dll)
- Linux: Native /dev/hidraw* with standard ioctl

License: MIT (Harmony Hub Control Project)
"""

from __future__ import annotations

import ctypes
import os
import platform
import struct
import sys
import time
from typing import Optional, Tuple

HARMONY_VID = 0x046D
HUB_PID = 0xC129
ELITE_PID = 0xC12B

REPORT_SIZE = 65  # 1 byte report ID (0x00) + 64 bytes payload


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

    def write_report(self, data: bytes) -> bool:
        """Write a 65-byte report (0x00 + 64 bytes)."""
        if len(data) == 64:
            buf = b"\x00" + data
        elif len(data) == 65:
            buf = data
        else:
            raise ValueError(f"Invalid report size: {len(data)}")

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
                    return False
                kernel32.GetOverlappedResult(self.handle, ctypes.byref(ov), ctypes.byref(written), False)
            else:
                return False
        return written.value == len(buf)

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
            else:
                return b""

        raw = buf.raw[: read.value]
        if len(raw) >= 65 and raw[0] == 0:
            return raw[1:65]
        return raw[:64]

    def close(self) -> None:
        if self.handle and self.handle not in (INVALID_HANDLE_VALUE, -1):
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
                elif f"pid_{HUB_PID:04x}" in lower or f"pid_{ELITE_PID:04x}" in lower:
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
                        if (target_pid and pid == target_pid) or (not target_pid and pid in (HUB_PID, ELITE_PID)):
                            return f"/dev/{os.path.basename(node)}"
        except Exception:
            continue
    return None


# ==============================================================================
# High-Level Harmony HID Bridge
# ==============================================================================

class HarmonyHid:
    def __init__(self, pid: Optional[int] = None):
        self.pid = pid
        self.dev = None
        self.seq = 1

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
            return True
        elif sys.platform.startswith("linux"):
            path = find_linux_device(self.pid)
            if not path:
                return False
            try:
                fd = os.open(path, os.O_RDWR)
                self.dev = LinuxHidDevice(fd, path)
                return True
            except OSError:
                return False
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
        s = self.seq
        self.seq = (self.seq + 1) & 0x7F
        if self.seq == 0:
            self.seq = 1
        return s

    def command(self, cmd_id: int, params: list[bytes], timeout_ms: int = 5000) -> bytes:
        """Send primary command frame and wait for matching response."""
        seq = self._next_seq()
        frames = build_primary_frames(cmd_id, seq, params)
        for f in frames:
            if not self.dev.write_report(f):
                raise IOError("HID report write failed")

        t_end = time.time() + (timeout_ms / 1000.0)
        while time.time() < t_end:
            resp = self.dev.read_report(int((t_end - time.time()) * 1000))
            if not resp:
                continue
            # Look for 0xFF cmd_id sequence ...
            if len(resp) >= 4 and resp[0] == 0xFF and resp[1] == cmd_id:
                return resp
            # Command error
            if len(resp) >= 4 and resp[0] == 0xFF and resp[1] == 0xFF:
                return resp
        return b""

    def open_file_write(self, remote_path: str, size: Optional[int] = None, timeout_ms: int = 15000) -> int:
        """Open a file for writing via command 0x01. Returns handle ID."""
        params = [encode_param_string(remote_path), encode_param_string("W")]
        if size is not None:
            params.append(encode_param_dword_be(size))

        resp = self.command(0x01, params, timeout_ms)
        if not resp or resp[0] != 0xFF or resp[1] != 0x01:
            raise RuntimeError(f"Open file '{remote_path}' failed: response {resp.hex() if resp else 'timeout'}")

        # Byte 5 carries the 1-byte handle ID
        # Format: FF 01 <seq> <params> 01 <handle_id> ...
        if len(resp) >= 6 and resp[4] == 0x01:
            return resp[5]
        return resp[2]  # fallback handle byte

    def close_file(self, handle_id: int, timeout_ms: int = 10000) -> bool:
        """Close an open file handle via command 0x04."""
        params = [encode_param_byte(handle_id)]
        resp = self.command(0x04, params, timeout_ms)
        return bool(resp and resp[0] == 0xFF and resp[1] == 0x04)

    def write_file_chunks(
        self,
        handle_id: int,
        data: bytes,
        packets_per_chunk: int = 5,
        timeout_ms: int = 15000,
        progress_callback=None,
    ) -> bool:
        """Stream data in chunks of data frames (cmd 0x03)."""
        data_frames = build_data_frames(data, chunk_size=62)
        total_data_frames = len(data_frames)
        sent_frames = 0

        while sent_frames < total_data_frames:
            burst = data_frames[sent_frames : sent_frames + packets_per_chunk]
            seq = self._next_seq()

            # Command 0x03 parameters: [handle_id, total_reports_in_burst]
            total_reports = 1 + len(burst)
            params = [encode_param_byte(handle_id), encode_param_word_be(total_reports)]
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
                if len(resp) >= 2 and resp[0] == 0xFF and resp[1] == 0x03:
                    acked = True
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
        """Complete workflow: open -> write chunks -> close."""
        handle_id = self.open_file_write(remote_path, size=len(data))
        try:
            self.write_file_chunks(handle_id, data, packets_per_chunk, progress_callback=progress_callback)
            return True
        finally:
            self.close_file(handle_id)

    def write_simple(self, remote_path: str, payload: bytes) -> bool:
        """Write a small single-burst endpoint (e.g. /tde/enable or /sys/reboot)."""
        handle_id = self.open_file_write(remote_path, size=len(payload))
        try:
            if payload:
                self.write_file_chunks(handle_id, payload, packets_per_chunk=1)
            return True
        finally:
            self.close_file(handle_id)

    # --------------------------------------------------------------------------
    # Specialized Hub / Elite Actions
    # --------------------------------------------------------------------------

    def set_tde_enable(self, state: bool = True) -> bool:
        """Set /etc/tdeenable on device via /tde/enable endpoint."""
        val = b"1" if state else b"0"
        return self.write_simple("/tde/enable", val)

    def reboot(self) -> bool:
        """Send reboot signal via /sys/reboot."""
        try:
            return self.write_simple("/sys/reboot", b"")
        except Exception:
            return True  # Device frequently resets USB bus immediately

    def provision_wifi(self, ssid: str, password: str, encryption: str = "WPA2-PSK") -> bool:
        """Provision Wi-Fi directly over USB HID."""
        payload = f"ssid,{ssid}\npassword,{password}\nencryption,{encryption}\n".encode("ascii")
        return self.write_simple("/sys/wifi/connect", payload)
