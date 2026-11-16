#!/usr/bin/env python3
"""USB root boot scripts refuse to run on the other device class.

The scripts staged by tools/root_hub_usb.py (Hub, MIPS) and
remote/setup_remote.py (remote, ARM) run here with sh in a sandbox: device
paths are moved under a temporary root, killall and uname are stubs, and the
script runs below a throwaway shell (the remote script kills its parent).
"""

from __future__ import annotations

import importlib.util
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools import root_hub_usb  # noqa: E402


def load_setup_remote():
    spec = importlib.util.spec_from_file_location("setup_remote", REPO_DIR / "remote" / "setup_remote.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


DEVICE_DIRS = "etc|root|home|usr|data|sbin|tmp|bin|sys"


@unittest.skipIf(sys.platform == "win32" or not shutil.which("sh"), "needs a POSIX sh")
class RootBootScripts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.root = base / "dev"
        (self.root / "etc").mkdir(parents=True)
        self.stub = base / "stub"
        self.stub.mkdir()
        for name, body in (("killall", "exit 0"), ("uname", 'echo "$FAKE_MACHINE"')):
            p = self.stub / name
            p.write_text(f"#!/bin/sh\n{body}\n")
            p.chmod(0o755)
        self.base = base

    def tearDown(self):
        self.tmp.cleanup()

    def run_script(self, template: bytes, machine: str) -> None:
        text = template.decode("ascii").replace("%PUBKEY%", "ssh-ed25519 AAAA test").replace("%OFFSET%", "00000000")
        text = re.sub(
            r"(?<![\w./$-])/(" + DEVICE_DIRS + r")(?=[/\s\"']|$)",
            lambda m: f"{self.root.as_posix()}/{m.group(1)}",
            text,
            flags=re.M,
        )
        script = self.base / "boot.sh"
        script.write_text(text)
        env = dict(os.environ, PATH=os.pathsep.join([str(self.stub), "/usr/bin", "/bin"]), FAKE_MACHINE=machine)
        subprocess.run(["sh", "-c", f'sh "{script}"; true'], env=env, capture_output=True, timeout=30)

    def changed(self) -> bool:
        return (self.root / "etc" / "nowatchdog").exists() or (self.root / "etc" / "dropbear").exists()

    def test_remote_script_on_hub(self):
        self.run_script(load_setup_remote().BOOTSTRAP_SCRIPT_TEMPLATE, "mips")
        self.assertFalse(self.changed())

    def test_remote_script_on_remote(self):
        self.run_script(load_setup_remote().BOOTSTRAP_SCRIPT_TEMPLATE, "armv5tejl")
        self.assertTrue(self.changed())

    def test_hub_script_on_remote(self):
        self.run_script(root_hub_usb.BOOTSTRAP_SCRIPT_TEMPLATE, "armv5tejl")
        self.assertFalse(self.changed())

    def test_hub_script_on_hub(self):
        self.run_script(root_hub_usb.BOOTSTRAP_SCRIPT_TEMPLATE, "mips")
        self.assertTrue(self.changed())


if __name__ == "__main__":
    unittest.main(verbosity=2)
