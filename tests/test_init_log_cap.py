#!/usr/bin/env python3
"""The log cap loop of payload/scripts/init.sh must cover every log the Hub keeps in /tmp.

The block after "# 9. Log cap" runs with sh in a sandbox: /tmp is moved under a temporary
root, the loop runs in the foreground, and a sleep stub sets the stop flag on its second
call, so exactly one pass runs. harmony-init.log and harmony-network.log start at 2 MB
each; both must end below the cap, and <log>.1 must hold only the last 64 KB (it held a copy of
the whole log, because the Hub's ash has no "command" builtin). Without dd no tail is kept.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
INIT = REPO_DIR / "payload" / "scripts" / "init.sh"
CAP = 262144
TAIL = 64 * 1024
LOGS = ("harmony-init.log", "harmony-network.log")


def cap_block() -> str:
    text = INIT.read_text()
    start = text.index("# 9. Log cap")
    end = text.index(") &\n", start)
    return text[start:end] + ")\n"


class InitLogCap(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmpdir.name)
        self.tmp = base / "tmp"
        self.tmp.mkdir()
        self.stub = base / "stub"
        self.stub.mkdir()
        sleep = self.stub / "sleep"
        sleep.write_text(
            "#!/bin/sh\n"
            f"if [ -f '{base}/slept' ]; then touch '{self.tmp}/harmony_stop'; else touch '{base}/slept'; fi\n"
        )
        sleep.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{self.stub}:/usr/bin:/bin")
        line = b"2026-11-14 04:00:00 [NET] Wi-Fi not associated, restarting wpa_supplicant\n"
        for name in LOGS:
            with open(self.tmp / name, "wb") as f:
                f.write(line * (2 * 1048576 // len(line) + 1))

    def tearDown(self):
        self.tmpdir.cleanup()

    def run_block(self, prefix: str = ""):
        block = re.sub(r"/tmp/", f"{self.tmp}/", cap_block())
        script = prefix + f'LOG="{self.tmp}/harmony-init.log"\n' + block
        res = subprocess.run(["sh", "-c", script], env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stderr)
        for name in LOGS:
            size = (self.tmp / name).stat().st_size
            self.assertLess(size, CAP, f"{name} was not capped ({size} bytes)")

    def test_all_tmp_logs_capped(self):
        self.run_block()
        for name in LOGS:
            tail = self.tmp / f"{name}.1"
            self.assertTrue(tail.is_file(), f"{name}.1 (the kept tail) is missing")
            self.assertLessEqual(tail.stat().st_size, TAIL + 1024, f"{name}.1 is more than the last 64 KB")

    def test_shell_without_command_builtin(self):
        """The Hub's stock ash has no "command" builtin (sh: command: not found): the block must
        not depend on it. Main's block then copied the whole log into <log>.1."""
        self.run_block('command() { echo "sh: command: not found" >&2; return 127; }\n')
        for name in LOGS:
            tail = self.tmp / f"{name}.1"
            self.assertTrue(tail.is_file(), f"{name}.1 (the kept tail) is missing")
            self.assertLessEqual(tail.stat().st_size, TAIL + 1024, f"{name}.1 is more than the last 64 KB")

    def test_no_dd_keeps_no_tail(self):
        dd = self.stub / "dd"
        dd.write_text("#!/bin/sh\nexit 127\n")
        dd.chmod(0o755)
        self.run_block()
        for name in LOGS:
            self.assertFalse((self.tmp / f"{name}.1").exists(), f"{name}.1 kept without dd")

    def test_no_command_builtin(self):
        # the Hub's stock ash answers "sh: command: not found"
        self.assertNotIn("command -v", cap_block())


if __name__ == "__main__":
    unittest.main(verbosity=2)
