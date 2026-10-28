#!/usr/bin/env python3
"""Unit tests for tools/usb_state.py."""

from __future__ import annotations

import pathlib
import sys
import unittest
from unittest.mock import MagicMock, patch

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools.usb_state import (
    UsbMode,
    UsbState,
    detect_remote_usb_state,
    print_usb_state_banner,
    require_firmware_or_exit,
)


class TestUsbState(unittest.TestCase):
    def test_state_defaults(self):
        s = UsbState(mode=UsbMode.DISCONNECTED)
        self.assertEqual(s.mode, UsbMode.DISCONNECTED)
        self.assertFalse(s.ip_valid)
        self.assertTrue(s.driver_ok)
        self.assertFalse(s.http_reachable)
        self.assertFalse(s.ssh_reachable)

    def test_banner_printing(self):
        """Ensure banner formatting executes without exception across all states."""
        for mode in [UsbMode.USB_HID, UsbMode.USB_ETHERNET, UsbMode.DRIVER_ERROR, UsbMode.DISCONNECTED]:
            s = UsbState(
                mode=mode,
                pid=0xC12B,
                description="Test Remote",
                adapter_name="Ethernet 2",
                ip_address="192.168.2.2" if mode == UsbMode.USB_ETHERNET else "169.254.1.1",
                ip_valid=(mode == UsbMode.USB_ETHERNET),
                driver_ok=(mode != UsbMode.DRIVER_ERROR),
                error_code=28 if mode == UsbMode.DRIVER_ERROR else None,
                error_details="Code 28 missing driver" if mode == UsbMode.DRIVER_ERROR else None,
                fw_version="4.15.330",
                fw_skin=99,
                fw_latest="4.15.330",
                fw_up_to_date=True,
                remediation_cmd="Set-StaticIP" if not (mode == UsbMode.USB_ETHERNET) else None,
            )
            print_usb_state_banner(s, label=f"TEST BANNER {mode}")

    def test_require_firmware_success(self):
        mock_hid = MagicMock()
        mock_hid.get_sysinfo.return_value = {
            "sw_version": "4.15.330",
            "skin": "99",
        }
        fw, skin = require_firmware_or_exit(mock_hid)
        self.assertEqual(fw, "4.15.330")
        self.assertEqual(skin, 99)

    def test_require_firmware_fallback_read(self):
        mock_hid = MagicMock()
        mock_hid.get_sysinfo.return_value = {}
        mock_hid.read_file.return_value = b"sw_version,4.15.260\nskin,116\n"

        fw, skin = require_firmware_or_exit(mock_hid, skin_default=116)
        self.assertEqual(fw, "4.15.260")
        self.assertEqual(skin, 116)

    def test_require_firmware_failure_aborts(self):
        mock_hid = MagicMock()
        mock_hid.get_sysinfo.return_value = {}
        mock_hid.read_file.return_value = b""

        with self.assertRaises(SystemExit) as cm:
            require_firmware_or_exit(mock_hid)
        self.assertEqual(cm.exception.code, 1)

    @patch("tools.usb_state._query_windows_state")
    @patch("tools.usb_state.probe_tcp")
    def test_windows_driver_error_detection(self, mock_tcp, mock_win):
        mock_tcp.return_value = False
        mock_win.return_value = (
            None,
            {
                "FriendlyName": "CDC Composite Gadget",
                "InstanceId": "USB\\VID_0525&PID_A4A2\\12345",
                "Status": "Error",
                "Problem": "CM_PROB_FAILED_INSTALL",
            },
            [],
            [],
        )

        with patch("sys.platform", "win32"):
            state = detect_remote_usb_state(check_firmware=False)
            self.assertEqual(state.mode, UsbMode.DRIVER_ERROR)
            self.assertFalse(state.driver_ok)
            self.assertEqual(state.error_code, 28)

    @patch("tools.usb_state._query_windows_state")
    @patch("tools.usb_state.probe_tcp")
    def test_windows_ip_remediation_detection(self, mock_tcp, mock_win):
        mock_tcp.return_value = False
        mock_win.return_value = (
            None,
            {
                "FriendlyName": "Remote NDIS Compatible Device",
                "InstanceId": "USB\\VID_0525&PID_A4A2\\12345",
                "Status": "OK",
                "Problem": "CM_PROB_NONE",
            },
            [{"Name": "Ethernet 3", "InterfaceDescription": "Remote NDIS Compatible Device"}],
            [{"InterfaceAlias": "Ethernet 3", "IPAddress": "169.254.12.34"}],
        )

        with patch("sys.platform", "win32"):
            state = detect_remote_usb_state(check_firmware=False)
            self.assertEqual(state.mode, UsbMode.USB_ETHERNET)
            self.assertTrue(state.driver_ok)
            self.assertFalse(state.ip_valid)
            self.assertIn("192.168.2.2", state.remediation_cmd)
            self.assertIn("Ethernet 3", state.remediation_cmd)


if __name__ == "__main__":
    unittest.main()
