#!/usr/bin/env python3
"""Hub reboot counter and emergency recovery mode, run on the host.

payload/scripts/rcS.local and safe_reboot.sh run with sh in a sandbox: every
absolute device path is moved under a temporary root, and the commands with
side effects (killall, reboot, mount, sleep, module loading, DHCP) are stubs
first in PATH. The stub init.sh never resets the counter, as on a Hub that
loses power within 5 minutes of each boot.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPTS = REPO_DIR / "payload" / "scripts"

DEVICE_DIRS = "data|tmp|etc|root|home|usr|sbin|bin|cache|proc|sys|opt|lib"
STUBS = ("killall", "reboot", "mount", "sleep", "sync", "load_modules", "udhcpc")


class BootCounter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name) / "root"
        self.stub = pathlib.Path(self.tmp.name) / "stub"
        self.stub.mkdir()
        for name in STUBS:
            p = self.stub / name
            p.write_text("#!/bin/sh\nexit 0\n")
            p.chmod(0o755)
        harmony = self.root / "data" / "harmony"
        harmony.mkdir(parents=True)
        (self.root / "tmp").mkdir()
        # Bluetooth init is not under test
        (harmony / "bt_disabled.conf").write_text("1\n")
        init = harmony / "init.sh"
        init.write_text("#!/bin/sh\nexit 0\n")
        init.chmod(0o755)
        self.counter = harmony / "reboot_counter"

    def tearDown(self):
        self.tmp.cleanup()

    def sandboxed(self, name: str) -> pathlib.Path:
        text = (SCRIPTS / name).read_text()
        text = re.sub(r"(?<![\w./$-])/(" + DEVICE_DIRS + r")/", lambda m: f"{self.root}/{m.group(1)}/", text)
        out = pathlib.Path(self.tmp.name) / name
        out.write_text(text)
        return out

    def run_script(self, name: str) -> None:
        env = dict(os.environ, PATH=f"{self.stub}:/usr/bin:/bin")
        subprocess.run(["sh", str(self.sandboxed(name))], env=env, capture_output=True, text=True, timeout=30)

    def boot(self) -> bool:
        """One boot with a fresh /tmp; True when it was an emergency boot."""
        for f in (self.root / "tmp").iterdir():
            if f.is_file():
                f.unlink()
        self.run_script("rcS.local")
        return (self.root / "tmp" / "emergency_recovery.log").exists()

    def test_emergency_mode_is_left_again(self):
        modes = [self.boot() for _ in range(10)]
        self.assertEqual(modes[:5], [False] * 5, "the first five short boots are normal")
        self.assertTrue(modes[5], "the sixth short boot is an emergency boot")
        self.assertFalse(modes[6], "the boot after an emergency boot starts normally")
        self.assertIn(False, modes[7:], "the Hub does not stay in emergency mode")

    def test_requested_reboot_does_not_count(self):
        self.counter.write_text("4\n")
        self.run_script("safe_reboot.sh")
        self.assertFalse(self.counter.exists())
        self.assertFalse(self.boot())
        self.assertEqual(self.counter.read_text().strip(), "1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
