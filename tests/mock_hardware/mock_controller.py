#!/usr/bin/env python3
"""
Harmony User-Space Mock Controller & Hardware Injector.
Provides test harness APIs to control virtual Remote and Hub hardware:
- Touchscreen injection (taps, swipes)
- Button press/release injection
- Framebuffer capture and inspection
- Gyroscope tilt injection
- Battery & Sysfs power status management
- Blasted IR waveform capture
"""

import os
import struct
import time

# Linux input event format: timeval (16 bytes on 64-bit), type (2B), code (2B), value (4B) = 24 bytes
# On 32-bit: timeval (8 bytes), type (2B), code (2B), value (4B) = 16 bytes
IS_64BIT = struct.calcsize("P") == 8
INPUT_EVENT_FMT = "llHHi" if IS_64BIT else "iiHHi"

# Event types
EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03
SYN_REPORT = 0x00

# ABS Axes
ABS_X = 0x00
ABS_Y = 0x01
ABS_PRESSURE = 0x18
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_PRESSURE = 0x3A

# Button Key Codes
KEY_POWER = 116
KEY_VOLUMEUP = 115
KEY_VOLUMEDOWN = 114
KEY_MUTE = 113
KEY_CHANNELUP = 402
KEY_CHANNELDOWN = 403
KEY_UP = 103
KEY_DOWN = 108
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_OK = 352
KEY_BACK = 158
KEY_MENU = 139
KEY_EXIT = 174

DEFAULT_MOCK_DIR = os.getenv("HARMONY_MOCK_DIR", "/tmp/harmony_mock")

class MockHardwareController:
    def __init__(self, mock_dir=DEFAULT_MOCK_DIR):
        self.mock_dir = mock_dir
        self.sysfs_dir = os.path.join(self.mock_dir, "sysfs")
        self.fb_path = os.path.join(self.mock_dir, "fb0")
        self.touch_pipe = os.path.join(self.mock_dir, "touch_pipe")
        self.btn_pipe = os.path.join(self.mock_dir, "btn_pipe")
        self.gyro_pipe = os.path.join(self.mock_dir, "gyro_pipe")
        self.ir_log_path = os.path.join(self.mock_dir, "ir_blaster.log")
        self._touch_fd = None
        self._btn_fd = None

    def setup_environment(self):
        os.makedirs(self.sysfs_dir, exist_ok=True)

        # Setup virtual framebuffer (240x320 RGB565 = 153600 bytes)
        if not os.path.exists(self.fb_path) or os.path.getsize(self.fb_path) < 153600:
            with open(self.fb_path, "wb") as f:
                f.write(b"\x00" * 153600)

        # Create named pipes for inputs if they don't exist
        for pipe_path in (self.touch_pipe, self.btn_pipe, self.gyro_pipe):
            if os.path.exists(pipe_path):
                try: os.unlink(pipe_path)
                except: pass
            os.mkfifo(pipe_path, 0o666)

        # Initialize default sysfs values
        self.set_power(vbat_mv=4150, charging=True, powersource="AC")
        self.set_backlight(100)
        self.set_screen_blank(False)
        self.set_gyro_xyz(0, 0, 980)

        # Clear IR log
        with open(self.ir_log_path, "w") as f:
            f.write("")

    def _pack_event(self, ev_type, code, val):
        now = time.time()
        sec = int(now)
        usec = int((now - sec) * 1000000)
        return struct.pack(INPUT_EVENT_FMT, sec, usec, ev_type, code, val)

    def _write_touch_event(self, ev_type, code, val):
        if self._touch_fd is None:
            self._touch_fd = os.open(self.touch_pipe, os.O_RDWR | os.O_NONBLOCK)
        os.write(self._touch_fd, self._pack_event(ev_type, code, val))

    def _write_btn_event(self, ev_type, code, val):
        if self._btn_fd is None:
            self._btn_fd = os.open(self.btn_pipe, os.O_RDWR | os.O_NONBLOCK)
        os.write(self._btn_fd, self._pack_event(ev_type, code, val))

    def tap_screen(self, x, y, duration_ms=50):
        """Simulate a complete finger tap on capacitive touchscreen."""
        # Touch Down
        self._write_touch_event(EV_ABS, ABS_X, x)
        self._write_touch_event(EV_ABS, ABS_Y, y)
        self._write_touch_event(EV_ABS, ABS_PRESSURE, 100)
        self._write_touch_event(EV_SYN, SYN_REPORT, 0)

        time.sleep(duration_ms / 1000.0)

        # Touch Up
        self._write_touch_event(EV_ABS, ABS_PRESSURE, 0)
        self._write_touch_event(EV_SYN, SYN_REPORT, 0)

    def swipe_screen(self, x1, y1, x2, y2, steps=10, duration_ms=150):
        """Simulate a finger drag/swipe gesture."""
        step_dt = (duration_ms / 1000.0) / steps
        for i in range(steps + 1):
            curr_x = int(x1 + (x2 - x1) * (i / steps))
            curr_y = int(y1 + (y2 - y1) * (i / steps))
            self._write_touch_event(EV_ABS, ABS_X, curr_x)
            self._write_touch_event(EV_ABS, ABS_Y, curr_y)
            self._write_touch_event(EV_ABS, ABS_PRESSURE, 100)
            self._write_touch_event(EV_SYN, SYN_REPORT, 0)
            time.sleep(step_dt)

        self._write_touch_event(EV_ABS, ABS_PRESSURE, 0)
        self._write_touch_event(EV_SYN, SYN_REPORT, 0)

    def press_button(self, key_code, duration_ms=80):
        """Simulate physical button press and release."""
        self._write_btn_event(EV_KEY, key_code, 1) # Press
        self._write_btn_event(EV_SYN, SYN_REPORT, 0)

        time.sleep(duration_ms / 1000.0)

        self._write_btn_event(EV_KEY, key_code, 0) # Release
        self._write_btn_event(EV_SYN, SYN_REPORT, 0)

    def set_power(self, vbat_mv=4150, charging=True, powersource="AC"):
        with open(os.path.join(self.sysfs_dir, "vbat"), "w") as f:
            f.write(f"{vbat_mv}\n")
        with open(os.path.join(self.sysfs_dir, "charger"), "w") as f:
            f.write("CHARGING\n" if charging else "DISCHARGING\n")
        with open(os.path.join(self.sysfs_dir, "powersource"), "w") as f:
            f.write(f"{powersource}\n")

    def set_backlight(self, val):
        with open(os.path.join(self.sysfs_dir, "backlight"), "w") as f:
            f.write(f"{val}\n")

    def set_screen_blank(self, blank=True):
        with open(os.path.join(self.sysfs_dir, "fb0_blank"), "w") as f:
            f.write("1\n" if blank else "0\n")

    def set_gyro_xyz(self, x, y, z):
        with open(os.path.join(self.sysfs_dir, "gyro_xyz"), "w") as f:
            f.write(f"{x} {y} {z}\n")

    def get_framebuffer_bytes(self):
        """Read current virtual 240x320 RGB565 framebuffer."""
        with open(self.fb_path, "rb") as f:
            return f.read(153600)

    def get_blasted_ir_records(self):
        """Retrieve IR signals recorded by mock blaster."""
        if not os.path.exists(self.ir_log_path):
            return []
        with open(self.ir_log_path, "r") as f:
            return [line.strip() for line in f if line.strip()]

    def close(self):
        if self._touch_fd:
            try: os.close(self._touch_fd)
            except: pass
            self._touch_fd = None
        if self._btn_fd:
            try: os.close(self._btn_fd)
            except: pass
            self._btn_fd = None
