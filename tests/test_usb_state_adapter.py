#!/usr/bin/env python3
"""tools/usb_state.py must find the remote's network adapter, not a phone's or a dongle's.

Phones in USB tethering mode and LTE sticks are RNDIS adapters too, and a USB Ethernet
dongle or dock is called enx... on Linux. The state check reports the adapter it picks as
the one to set to 192.168.2.2, so a wrong pick tells the user to reconfigure their
Internet link. No device, no network: Windows answers and Linux sysfs are faked.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools import usb_state  # noqa: E402
from tools.usb_state import UsbMode, detect_remote_usb_state  # noqa: E402

REMOTE_PNP = {"FriendlyName": "Remote NDIS Compatible Device", "InstanceId": "USB\\VID_0525&PID_A4A2\\12345",
              "Status": "OK", "Problem": "CM_PROB_NONE"}
PHONE = {"Name": "Ethernet 2", "InterfaceDescription": "Remote NDIS based Internet Sharing Device",
         "PnPDeviceID": "USB\\VID_18D1&PID_4EE4&MI_00\\6&1"}
REMOTE = {"Name": "Ethernet 3", "InterfaceDescription": "Remote NDIS Compatible Device",
          "PnPDeviceID": "USB\\VID_0525&PID_A4A2\\7&2"}
IPS = [{"InterfaceAlias": "Ethernet 2", "IPAddress": "192.168.43.10"},
       {"InterfaceAlias": "Ethernet 3", "IPAddress": "169.254.12.34"}]


class WindowsAdapter(unittest.TestCase):
    @patch("tools.harmony_hid.HarmonyHid.open", return_value=False)
    @patch("tools.usb_state._query_windows_state")
    @patch("tools.usb_state.probe_tcp", return_value=False)
    def test_phone_listed_first_is_not_taken(self, _tcp, mock_win, _hid):
        mock_win.return_value = (None, REMOTE_PNP, [PHONE, REMOTE], IPS)
        with patch("sys.platform", "win32"):
            state = detect_remote_usb_state(check_firmware=False)
        self.assertEqual(state.mode, UsbMode.USB_ETHERNET)
        self.assertEqual(state.adapter_name, "Ethernet 3")
        self.assertNotIn("Ethernet 2", state.remediation_cmd or "")

    @patch("tools.harmony_hid.HarmonyHid.open", return_value=False)
    @patch("tools.usb_state._query_windows_state")
    @patch("tools.usb_state.probe_tcp", return_value=False)
    def test_only_a_phone(self, _tcp, mock_win, _hid):
        mock_win.return_value = (None, REMOTE_PNP, [PHONE], IPS[:1])
        with patch("sys.platform", "win32"):
            state = detect_remote_usb_state(check_firmware=False)
        self.assertNotEqual(state.adapter_name, "Ethernet 2", "the phone's adapter was reported as the remote's")

    @patch("tools.harmony_hid.HarmonyHid.open", return_value=False)
    @patch("tools.usb_state._windows_static_ips", return_value={"192.168.2.2", "192.168.43.10"})
    @patch("tools.usb_state._query_windows_state")
    @patch("tools.usb_state.probe_tcp", return_value=False)
    def test_second_remote_adapter(self, _tcp, mock_win, _used, _hid):
        """Each remote gets its own adapter. The hidden adapter of a remote set up before keeps
        192.168.2.2, so the new adapter is told to take the next free address."""
        new = dict(REMOTE, Status="Up", InterfaceGuid="{NEW}")
        ips = [{"InterfaceAlias": "Ethernet 3", "IPAddress": "192.168.2.2", "AddressState": "Duplicate"}]
        mock_win.return_value = (None, REMOTE_PNP, [new], ips)
        with patch("sys.platform", "win32"):
            state = detect_remote_usb_state(check_firmware=False)
        _used.assert_called_once_with("{NEW}")
        self.assertEqual(state.adapter_name, "Ethernet 3")
        self.assertFalse(state.ip_valid)
        self.assertEqual(state.remediation_cmd,
                         'Remove-NetIPAddress -InterfaceAlias "Ethernet 3" -IPAddress 192.168.2.2 -Confirm:$false; '
                         'New-NetIPAddress -InterfaceAlias "Ethernet 3" -IPAddress 192.168.2.3 -PrefixLength 24')

    @patch("tools.harmony_hid.HarmonyHid.open", return_value=False)
    @patch("tools.usb_state._windows_static_ips", return_value={"192.168.2.2"})
    @patch("tools.usb_state._query_windows_state")
    @patch("tools.usb_state.probe_tcp", return_value=False)
    def test_own_address_next_to_another_remote(self, _tcp, mock_win, _used, _hid):
        old = dict(REMOTE, Name="Ethernet 4", Status="Disconnected", PnPDeviceID="USB\\VID_0525&PID_A4A2\\7&9")
        new = dict(REMOTE, Status="Up", InterfaceGuid="{NEW}")
        ips = [{"InterfaceAlias": "Ethernet 3", "IPAddress": "192.168.2.3", "AddressState": "Preferred"}]
        mock_win.return_value = (None, REMOTE_PNP, [old, new], ips)
        with patch("sys.platform", "win32"):
            state = detect_remote_usb_state(check_firmware=False)
        self.assertEqual(state.adapter_name, "Ethernet 3", "the Disconnected adapter was taken")
        self.assertTrue(state.ip_valid)
        self.assertIsNone(state.remediation_cmd)

    def test_free_host_ip(self):
        self.assertEqual(usb_state.free_host_ip(set()), "192.168.2.2")
        self.assertEqual(usb_state.free_host_ip({"192.168.2.2", "192.168.2.3"}), "192.168.2.4")

    def test_no_device_ids_falls_back_to_the_name(self):
        ad = {"Name": "Ethernet 3", "InterfaceDescription": "Remote NDIS Compatible Device"}
        self.assertEqual(usb_state._match_windows_adapter(REMOTE_PNP, [ad]), ad)


class LinuxAdapter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        self.net = self.base / "net"
        self.net.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def add_usb_if(self, name: str, vid: str, pid: str) -> None:
        usbdev = self.base / "usb" / name
        iface = usbdev / f"{name}:1.0"
        iface.mkdir(parents=True)
        (usbdev / "idVendor").write_text(vid + "\n")
        (usbdev / "idProduct").write_text(pid + "\n")
        (self.net / name).mkdir()
        (self.net / name / "device").symlink_to(iface)

    def query(self, addrs: dict[str, str]):
        def fake_run(cmd, *a, **k):
            ifname = cmd[-1]
            info = [{"addr_info": [{"family": "inet", "local": addrs[ifname]}]}] if ifname in addrs else [{"addr_info": []}]
            return subprocess.CompletedProcess(cmd, 0, json.dumps(info), "")
        with patch.object(usb_state.subprocess, "run", side_effect=fake_run):
            return usb_state._query_linux_network(self.net)

    def test_dongle_is_not_the_remote(self):
        self.add_usb_if("enx001122334455", "0bda", "8153")  # a USB 3 Ethernet dongle
        found_if, _, _ = self.query({"enx001122334455": "192.168.1.20"})
        self.assertIsNone(found_if, "a USB Ethernet dongle was taken for the remote")

    def test_remote_found_next_to_a_dongle(self):
        self.add_usb_if("enx001122334455", "0bda", "8153")
        self.add_usb_if("usb0", "0525", "a4a2")
        found_if, ip, ok = self.query({"enx001122334455": "192.168.1.20"})
        self.assertEqual(found_if, "usb0")
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main(verbosity=2)
