#!/usr/bin/env python3
"""init.sh rewrites its profile files, /usr/sbin/dropbear and its links only when they change.

/ on the Hub is a union over /data, so each rewrite is a flash write, and a power cut while a
file is rewritten could leave it empty. The helper functions and the block "# 3. Shell Profile"
of payload/scripts/init.sh run twice with sh in a sandbox (paths moved into a temporary root, the
SSH wrapper hub_sshd and a stand-in server binary, ps and dropbear start replaced): the first run
writes every file and link; the second leaves all of them untouched (same inode, same modification
time), also without a readlink command (the stock BusyBox has none).
"""

from __future__ import annotations

import os
import re
import pathlib
import subprocess
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
INIT = REPO_DIR / "payload" / "scripts" / "init.sh"


def block() -> str:
    text = INIT.read_text()
    h = text.find("write_if_changed() {")
    helpers = text[h: text.index('echo "$(date) harmony init start')] if h >= 0 else ""
    start = text.index("# 3. Shell Profile")
    end = text.index("# 4. Disable")
    return helpers + text[start:end]


class InitFlashWrites(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        r = self.root
        for d in ("data/harmony/bin", "cache/bin", "etc", "root", "home/root", "usr/sbin", "stub"):
            (r / d).mkdir(parents=True)
        (r / "data" / "harmony" / "bin" / "dropbear").write_text("#!/bin/sh\n")
        (r / "data" / "harmony" / "bin" / "dropbear").chmod(0o755)
        wrapper = (REPO_DIR / "payload" / "scripts" / "hub_sshd").read_text()
        wrapper = re.sub(r"(?<![\w/.-])/(data|cache|usr)/", lambda m: f"{r}{m.group(0)}", wrapper)
        (r / "data" / "harmony" / "hub_sshd").write_text(wrapper)
        (r / "data" / "harmony" / "hub_sshd").chmod(0o755)
        (r / "data" / "harmony" / "safe_reboot.sh").write_text("#!/bin/sh\n")
        # ps: a dropbear is running, so the block does not start one
        (r / "stub" / "ps").write_text("#!/bin/sh\necho '  1 root dropbear'\n")
        (r / "stub" / "ps").chmod(0o755)
        text = block()
        text = re.sub(r"(?<![\w/.-])/(data|cache|etc|root|home|usr)/", lambda m: f"{r}{m.group(0)}", text)
        self.script = r / "block.sh"
        self.script.write_text(text)
        self.env = dict(os.environ, PATH=f"{r}/stub:/usr/bin:/bin")
        self.paths = [
            r / "etc" / "profile.d" / "00-harmony.sh",
            r / "root" / ".profile",
            r / "home" / "root" / ".profile",
            r / "usr" / "sbin" / "dropbear",
            r / "data" / "harmony" / "bin" / "sync",
            r / "cache" / "bin" / "sync",
            r / "data" / "harmony" / "bin" / "safe_reboot.sh",
        ]

    def tearDown(self):
        self.tmp.cleanup()

    def run_block(self):
        res = subprocess.run(["sh", str(self.script)], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(res.returncode, 0, res.stderr)

    def state(self):
        return {str(p): (os.lstat(p).st_ino, os.lstat(p).st_mtime_ns) for p in self.paths}

    def test_second_start_writes_nothing(self):
        self.run_block()
        for p in self.paths:
            self.assertTrue(os.path.lexists(p), p)
        self.assertFalse((self.root / "usr" / "sbin" / "dropbear").is_symlink())
        self.assertIn("-s -g -K 300", (self.root / "usr" / "sbin" / "dropbear").read_text())
        self.assertEqual((self.root / "usr" / "sbin" / "dropbear").stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.root / "root" / ".profile").stat().st_mode & 0o777, 0o644)
        before = self.state()
        time.sleep(0.05)
        self.run_block()
        self.assertEqual(self.state(), before, "files or links rewritten although unchanged")
        self.assertFalse(list(self.root.rglob("*.tmp")), "temporary files left")

    def test_links_kept_without_readlink(self):
        # the stock BusyBox has no readlink: a missing one must not make every start relink
        ro = self.root / "stub" / "readlink"
        ro.write_text("#!/bin/sh\nexit 127\n")
        ro.chmod(0o755)
        self.run_block()
        before = self.state()
        time.sleep(0.05)
        self.run_block()
        self.assertEqual(self.state(), before, "links rewritten although unchanged (no readlink)")

    def test_changed_content_is_replaced(self):
        self.run_block()
        prof = self.root / "etc" / "profile.d" / "00-harmony.sh"
        prof.write_text("stale\n")
        self.run_block()
        self.assertIn("export PATH=", prof.read_text())


if __name__ == "__main__":
    unittest.main()
