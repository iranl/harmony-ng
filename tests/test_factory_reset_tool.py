#!/usr/bin/env python3
"""tools/factory_reset.py must report a reset that did not complete.

The tool runs as a subprocess with a fake ssh first in PATH (no network, nothing run).
The fake answers the probe with a given machine name, can fail (exit 255, as a dropped
connection does) from a given call on, or end the reset login after a given step, and
logs every command it answered. A reset that did not complete must not be reported as
done; all steps and the reboot must go in one login (the remote's watchdog runs out
10 s after its runtime stops); the tools must refuse the wrong kind of device; --target
and a Hub address are required; the Hub is not reset when its backup fails.
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
if [ -n "$FAKE_FAIL_FROM" ] && [ "$n" -ge "$FAKE_FAIL_FROM" ]; then
  echo "Connection to device closed." >&2; exit 255
fi
printf '%s\\n' "$last" >> "$FAKE_DIR/log"
case "$last" in
  "uname -m") echo "$FAKE_MACHINE" ;;
  *factory-reset-steps-done*)
    cat > /dev/null  # the reset script arrives on stdin
    if [ -n "$FAKE_CUT_AFTER" ]; then
      i=1; while [ "$i" -le "$FAKE_CUT_AFTER" ]; do echo "factory-reset-step $i"; i=$((i + 1)); done
      echo "Connection to device closed." >&2; exit 255
    fi
    # The steps are not run here: print the marks the device would print
    steps=$(printf '%s' "$last" | grep -o 'echo factory-reset-step [0-9]*' | wc -l)
    i=1; while [ "$i" -le "$steps" ]; do echo "factory-reset-step $i"; i=$((i + 1)); done
    echo "factory-reset-steps-done" ;;
esac
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
                        HOME=str(base), FAKE_MACHINE="armv5tejl")
        self.env.pop("HUB_IP", None)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, args: list[str], fail_from: int | None = None, **env_extra) -> subprocess.CompletedProcess[str]:
        env = dict(self.env, FAKE_FAIL_FROM=str(fail_from or ""), **env_extra)
        return subprocess.run([sys.executable, str(TOOL), "--force"] + args,
                              env=env, capture_output=True, text=True, timeout=60)

    def commands(self) -> list[str]:
        log = self.base / "log"
        return log.read_text().splitlines() if log.exists() else []

    def test_remote_login_fails(self):
        res = self.run_tool(["--target", "remote"], fail_from=2)  # probe ok, the reset login fails
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertNotIn("completed successfully", res.stdout)

    def test_remote_cut_mid_way(self):
        res = self.run_tool(["--target", "remote"], FAKE_CUT_AFTER="2")
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertIn("was not confirmed", res.stdout)
        self.assertNotIn("completed successfully", res.stdout)

    def test_hub_unreachable(self):
        res = self.run_tool(["--target", "hub", "--hub-ip", "hub.invalid", "--no-backup"], fail_from=1)
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertNotIn("completed successfully", res.stdout)

    def test_good_reset_is_one_login(self):
        res = self.run_tool(["--target", "remote"])
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("completed successfully", res.stdout)
        cmds = self.commands()
        self.assertEqual(len(cmds), 2, "probe and one login for every step and the reboot")
        self.assertIn("reboot -f", cmds[-1])
        self.assertLess(cmds[-1].index("factory-reset-steps-done"), cmds[-1].index("reboot -f"))

    def test_target_required(self):
        res = self.run_tool([])
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("--target is required", res.stderr)
        self.assertEqual(self.commands(), [])

    def test_hub_address_required(self):
        res = self.run_tool(["--target", "hub", "--no-backup"])
        self.assertNotEqual(res.returncode, 0)
        self.assertEqual(self.commands(), [], "logged in somewhere without a Hub address")

    def test_hub_reset_refuses_the_remote(self):
        res = self.run_tool(["--target", "hub", "--hub-ip", "192.168.2.1", "--no-backup"])
        self.assertNotEqual(res.returncode, 0)
        self.assertEqual(self.commands(), ["uname -m"])

    def test_remote_reset_refuses_the_hub(self):
        res = self.run_tool(["--target", "remote"], FAKE_MACHINE="mips")
        self.assertNotEqual(res.returncode, 0)
        self.assertEqual(self.commands(), ["uname -m"])

    def test_hub_reset_without_backup_does_nothing(self):
        # The fake streams no archive, so the backup fails: /data/harmony must stay
        res = self.run_tool(["--target", "hub", "--hub-ip", "hub.invalid"], FAKE_MACHINE="mips",
                            SSH_KEY="/nonexistent")
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertFalse([c for c in self.commands() if "rm -rf /data/harmony" in c])
        self.assertIn("No backup was made", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
