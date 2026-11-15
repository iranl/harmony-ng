#!/usr/bin/env python3
"""payload/scripts/factory_reset.sh on the host, paths moved into a temporary root and
killall, mount, reboot and sleep replaced by stubs that record their calls.

- the init.sh supervisor loops are stopped before the daemons are killed (they would start
  them again 5 s later while the script deletes their files);
- the daemons are killed (kill -9, .agents/rules/embedded_devices.md) and 2 s pass before
  the first flash write, as in safe_reboot.sh;
- the file systems are flushed with hub_sync (kept in RAM before /data/harmony/bin is
  removed, executable also under a umask that clears x) and remounted read-only before
  "reboot -f"; the old path of an earlier project name is gone;
- when the stock rcS.local cannot be restored nothing is removed and the services are started
  again.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "factory_reset.sh"


class FactoryResetScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        r = self.root
        for d in ("tmp", "etc/init.d", "data/harmony/bin", "data/harmony/config", "data/stock_backup",
                  "cache/bin", "usr/sbin", "stub"):
            (r / d).mkdir(parents=True)
        text = SCRIPT.read_text()
        # every absolute device path, but not the #! line
        text = re.sub(r"(?<![\w./$!-])/(tmp|data|etc|cache|usr|opt|mnt|root|home)/", lambda m: f"{r}{m.group(0)}", text)
        self.script = r / "factory_reset.sh"
        self.script.write_text(text)
        self.calls = r / "calls.log"
        for cmd in ("killall", "mount", "reboot", "sleep"):
            f = r / "stub" / cmd
            f.write_text(f"#!/bin/sh\necho \"{cmd} $*\" >> '{self.calls}'\nexit 0\n")
            f.chmod(0o755)
        hub_util = r / "data" / "harmony" / "bin" / "hub_util"
        hub_util.write_text(f"#!/bin/sh\necho \"flush $(basename $0)\" >> '{self.calls}'\n")
        hub_util.chmod(0o755)
        init = r / "data" / "harmony" / "init.sh"
        init.write_text(f"#!/bin/sh\necho \"init.sh $*\" >> '{self.calls}'\n")
        init.chmod(0o755)
        (r / "data" / "stock_backup" / "rcS.local.orig").write_text("#!/bin/sh\n# stock\n")
        self.env = dict(os.environ, PATH=f"{r}/stub:/usr/bin:/bin")
        self.loop = subprocess.Popen(["sleep", "60"])
        (r / "tmp" / "harmony_supervisors.pid").write_text(f"{self.loop.pid}\n")

    def tearDown(self):
        if self.loop.poll() is None:
            self.loop.kill()
        self.loop.wait()
        self.tmp.cleanup()

    def run_reset(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["sh", str(self.script), "--confirm"], env=self.env, capture_output=True,
                              text=True, timeout=60)

    def test_stop_flush_remount_reboot(self):
        res = self.run_reset()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.loop.wait(timeout=5)
        self.assertTrue((self.root / "tmp" / "harmony_stop").exists(), "the stop flag is set")
        calls = self.calls.read_text().splitlines()
        self.assertIn("flush hub_sync", calls)
        reboot = calls.index("reboot -f")
        remount = [i for i, c in enumerate(calls) if c.startswith("mount -o remount,ro") and "/data" in c]
        self.assertTrue(remount and remount[0] < reboot, calls)
        self.assertLess(calls.index("flush hub_sync"), reboot)
        kill = next(i for i, c in enumerate(calls) if c.startswith("killall -9 init.sh"))
        self.assertEqual(calls[kill + 1], "sleep 2", "a wait for SPI transfers in flight after the kill")
        self.assertNotIn("codex", SCRIPT.read_text())
        self.assertFalse((self.root / "data" / "harmony" / "bin").exists())

    def test_flush_copy_runs_with_a_strict_umask(self):
        # cp gives the copy the source's mode minus the umask: with one that clears x the RAM copy
        # of hub_sync could not run and the flush fell back to a plain sync
        res = subprocess.run(["sh", "-c", f"umask 177; exec sh '{self.script}' --confirm"], env=self.env,
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("flush hub_sync", self.calls.read_text().splitlines())

    def test_failed_restore_removes_nothing(self):
        (self.root / "etc" / "init.d").rmdir()  # cp to /etc/init.d/rcS.local fails
        res = self.run_reset()
        self.assertNotEqual(res.returncode, 0)
        self.assertTrue((self.root / "data" / "harmony" / "bin" / "hub_util").exists(), "nothing removed")
        self.assertFalse((self.root / "tmp" / "harmony_stop").exists(), "the stop flag is cleared again")
        for _ in range(50):
            if self.calls.exists() and "init.sh start" in self.calls.read_text():
                break
            import time
            time.sleep(0.1)
        calls = self.calls.read_text().splitlines()
        self.assertIn("init.sh start", calls)
        self.assertNotIn("reboot -f", calls)


if __name__ == "__main__":
    unittest.main()
