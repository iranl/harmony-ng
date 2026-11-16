#!/usr/bin/env python3
"""payload/scripts/start_usb_eth.sh registers the EHCI platform device at most once per boot.

register_ehci patches a system call entry to register a static platform device. When ehci-hcd
did not load, the device is registered but no driver is bound, and the old test (driver bound?)
let every later start register the same device again. The script runs in a sandbox: /sys, /tmp,
/lib and the module paths are moved into a temporary root, insmod and the other commands are
stubs, and the register_ehci stub records each call and creates the device directory without a
driver (or, in the second case, fails without creating it).
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


class StartUsbEthOnce(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        r = self.root = pathlib.Path(self.tmp.name)
        for d in ("tmp", "sys/devices/platform", "data/harmony/bin", "stub"):
            (r / d).mkdir(parents=True)
        text = re.sub(r"(?<![\w/.-])/(sys|tmp|lib|data|cache|usr)/", lambda m: f"{r}{m.group(0)}", SCRIPT.read_text())
        self.script = r / "start_usb_eth.sh"
        self.script.write_text(text)
        for cmd in ("insmod", "rmmod", "killall", "umount", "sleep"):
            f = r / "stub" / cmd
            f.write_text("#!/bin/sh\nexit 0\n")
            f.chmod(0o755)
        self.calls = r / "register_calls"
        self.env = dict(os.environ, PATH=f"{r}/stub:/usr/bin:/bin")

    def tearDown(self):
        self.tmp.cleanup()

    def make_register(self, creates_device: bool):
        dev = self.root / "sys" / "devices" / "platform" / "ar7240-ehci.0"
        body = f"echo call >> '{self.calls}'\n"
        body += f"mkdir -p '{dev}'\nexit 0\n" if creates_device else "exit 1\n"
        f = self.root / "data" / "harmony" / "bin" / "register_ehci"
        f.write_text("#!/bin/sh\n" + body)
        f.chmod(0o755)

    def run_script(self) -> str:
        res = subprocess.run(["sh", str(self.script)], env=self.env, capture_output=True, text=True, timeout=60)
        return res.stdout

    def calls_made(self) -> int:
        return len(self.calls.read_text().split()) if self.calls.exists() else 0

    def test_registered_device_without_driver_not_registered_again(self):
        self.make_register(creates_device=True)
        out = self.run_script()
        self.assertIn("register_ehci exit code 0", out)
        self.run_script()
        self.run_script()
        self.assertEqual(self.calls_made(), 1)

    def test_failed_registration_is_retried(self):
        self.make_register(creates_device=False)
        self.assertIn("register_ehci exit code 1", self.run_script())
        self.run_script()
        self.assertEqual(self.calls_made(), 2)


if __name__ == "__main__":
    unittest.main()
