#!/usr/bin/env python3
"""tools/factory_reset.py must report a reset that did not complete.

The tool runs as a subprocess with a fake ssh first in PATH (no network). The fake
answers the probe, then fails (exit 255, as a dropped connection does) from a given call
on, and logs every command. A failed step must stop the reset before the reboot, and
the tool must exit non-zero without the success message; an unreachable device must not
end in "completed successfully" either.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
TOOL = REPO_DIR / "tools" / "factory_reset.py"

FAKE_SSH = """#!/bin/sh
for last; do :; done
n=$(cat "$FAKE_DIR/count" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$FAKE_DIR/count"
printf '%s\\n' "$last" >> "$FAKE_DIR/log"
if [ -n "$FAKE_FAIL_FROM" ] && [ "$n" -ge "$FAKE_FAIL_FROM" ]; then
  echo "Connection to device closed." >&2; exit 255
fi
[ "$last" = "uname -a" ] && echo "Linux device"
exit 0
"""


class FactoryResetTool(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        (base / "bin").mkdir()
        ssh = base / "bin" / "ssh"
        ssh.write_text(FAKE_SSH)
        ssh.chmod(0o755)
        self.base = base
        self.env = dict(os.environ, PATH=f"{base / 'bin'}:{os.environ['PATH']}", FAKE_DIR=str(base),
                        HOME=str(base))

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, target: str, fail_from: int | None) -> subprocess.CompletedProcess[str]:
        env = dict(self.env, FAKE_FAIL_FROM=str(fail_from or ""))
        return subprocess.run([sys.executable, str(TOOL), "--target", target, "--force"],
                              env=env, capture_output=True, text=True, timeout=60)

    def commands(self) -> list[str]:
        log = self.base / "log"
        return log.read_text().splitlines() if log.exists() else []

    def test_remote_step_fails(self):
        res = self.run_tool("remote", fail_from=3)  # probe ok, step 1 ok, step 2 fails
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertNotIn("completed successfully", res.stdout)
        self.assertFalse([c for c in self.commands() if "reboot -f" in c],
                         "the remote must not be rebooted after a failed reset step")

    def test_hub_unreachable(self):
        res = self.run_tool("hub", fail_from=1)
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertNotIn("completed successfully", res.stdout)

    def test_good_reset(self):
        res = self.run_tool("remote", fail_from=None)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("completed successfully", res.stdout)
        self.assertTrue("reboot -f" in self.commands()[-1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
