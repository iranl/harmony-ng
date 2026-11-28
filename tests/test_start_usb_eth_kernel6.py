#!/usr/bin/env python3
"""payload/scripts/start_usb_eth.sh on the Linux 6.18 Hub system (build/kernel6).

There the USB controller is a ChipIdea one whose role is set in sysfs, the adapter drivers come
from the kernel's own modules (modprobe), the stock-kernel steps (ath_udc, register_ehci) are
skipped, and a USB adapter that got eth0 or eth1 is renamed eth2 or eth3, because
network_manager.sh keeps eth0 and eth1 for the onboard ports. The script runs in a sandbox as
in test_start_usb_eth_once.py.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "start_usb_eth.sh"


class StartUsbEthKernel6(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        r = self.root = pathlib.Path(self.tmp.name)
        self.role = r / "sys/bus/platform/devices/1b000000.usb/ci_hdrc.0/role"
        self.role.parent.mkdir(parents=True)
        self.role.write_text("gadget\n")
        usbdev = r / "sys/bus/usb/devices/1-1"
        usbdev.mkdir(parents=True)
        (usbdev / "idVendor").write_text("0bda\n")
        (usbdev / "idProduct").write_text("8152\n")
        (usbdev / "speed").write_text("480\n")
        nicdev = r / "sys/devices/platform/ahb/1b000000.usb/usb1/1-1/1-1:1.0"
        nicdev.mkdir(parents=True)
        net = r / "sys/class/net/eth0"
        net.mkdir(parents=True)
        (net / "device").symlink_to(nicdev)
        for d in ("tmp", "data/harmony/bin", "stub"):
            (r / d).mkdir(parents=True, exist_ok=True)
        text = re.sub(r"(?<![\w/.-])/(sys|tmp|lib|data|cache|usr)/", lambda m: f"{r}{m.group(0)}", SCRIPT.read_text())
        self.script = r / "start_usb_eth.sh"
        self.script.write_text(text)
        self.log = r / "calls"
        for cmd in ("insmod", "rmmod", "killall", "umount", "sleep", "modprobe", "ip"):
            f = r / "stub" / cmd
            f.write_text(f"#!/bin/sh\necho {cmd} \"$@\" >> '{self.log}'\nexit 0\n")
            f.chmod(0o755)
        reg = r / "data/harmony/bin/register_ehci"
        reg.write_text(f"#!/bin/sh\necho register_ehci >> '{self.log}'\n")
        reg.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{r}/stub:/usr/bin:/bin")

    def tearDown(self):
        self.tmp.cleanup()

    def test_host_role_modprobe_and_rename(self):
        subprocess.run(["sh", str(self.script)], env=self.env, capture_output=True, text=True, timeout=120)
        calls = self.log.read_text().splitlines()
        self.assertEqual(self.role.read_text().strip(), "host")
        self.assertIn("modprobe r8152", calls)
        self.assertIn("ip link set eth0 name eth2", calls)
        self.assertNotIn("register_ehci", calls)
        self.assertFalse(any(c.startswith("insmod") for c in calls), calls)
        self.assertEqual((self.root / "tmp/harmony_usb_port_mode").read_text().strip(), "host")

    def test_adapter_behind_a_usb_hub_next_to_a_stick(self):
        # 1-1 is a USB 2.0 hub (class 09); the stick (1-1.1, storage only) comes first, the adapter is 1-1.2
        devs = self.root / "sys/bus/usb/devices"
        for name, vid, pid, cls, ifcls in (("1-1", "05e3", "0608", "09", "09"),
                                           ("1-1.1", "0781", "5567", "00", "08"),
                                           ("1-1.2", "0b95", "1790", "00", "ff")):
            d = devs / name
            d.mkdir(parents=True, exist_ok=True)
            (d / "idVendor").write_text(vid + "\n")
            (d / "idProduct").write_text(pid + "\n")
            (d / "speed").write_text("480\n")
            (d / "bDeviceClass").write_text(cls + "\n")
            (d / f"{name}:1.0").mkdir(exist_ok=True)
            (d / f"{name}:1.0" / "bInterfaceClass").write_text(ifcls + "\n")
        subprocess.run(["sh", str(self.script)], env=self.env, capture_output=True, text=True, timeout=120)
        calls = self.log.read_text().splitlines()
        self.assertIn("modprobe ax88179_178a", calls)
        self.assertNotIn("modprobe cdc_ether", calls)


if __name__ == "__main__":
    unittest.main()
