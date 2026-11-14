#!/usr/bin/env python3
"""The log cap loop of payload/scripts/init.sh must cover every log the Hub keeps in /tmp.

The block after "# 9. Log cap" runs with sh in a sandbox: /tmp is moved under a temporary
root, the loop runs in the foreground, and a sleep stub sets the stop flag on its second
call, so exactly one pass runs. harmony-init.log, harmony-network.log and
harmony-recovery.log start at 2 MB each; all three must end below the cap.
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
CAP = 1048576
LOGS = ("harmony-init.log", "harmony-network.log", "harmony-recovery.log")


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
        stub = base / "stub"
        stub.mkdir()
        sleep = stub / "sleep"
        sleep.write_text(
            "#!/bin/sh\n"
            f"if [ -f '{base}/slept' ]; then touch '{self.tmp}/harmony_stop'; else touch '{base}/slept'; fi\n"
        )
        sleep.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{stub}:/usr/bin:/bin")
        line = b"2026-11-14 04:00:00 [NET] Wi-Fi not associated, restarting wpa_supplicant\n"
        for name in LOGS:
            with open(self.tmp / name, "wb") as f:
                f.write(line * (2 * CAP // len(line) + 1))

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_all_tmp_logs_capped(self):
        block = re.sub(r"/tmp/", f"{self.tmp}/", cap_block())
        script = f'LOG="{self.tmp}/harmony-init.log"\n' + block
        res = subprocess.run(["sh", "-c", script], env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stderr)
        for name in LOGS:
            size = (self.tmp / name).stat().st_size
            self.assertLess(size, CAP, f"{name} was not capped ({size} bytes)")
            self.assertTrue((self.tmp / f"{name}.1").is_file(), f"{name}.1 (the kept tail) is missing")


if __name__ == "__main__":
    unittest.main(verbosity=2)
