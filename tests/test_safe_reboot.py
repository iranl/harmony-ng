#!/usr/bin/env python3
"""payload/scripts/safe_reboot.sh must stop the init.sh supervisor loops, run on the host.

The script runs with sh in a sandbox: every absolute device path is moved under a
temporary root, and killall, mount, sleep, sync and reboot are stubs first in PATH.
A stand-in supervisor loop (the shape of the init.sh loops: exit on /tmp/harmony_stop,
otherwise keep restarting the daemon) is started and its pid recorded in
/tmp/harmony_supervisors.pid, as init.sh does. When reboot -f runs, the loop must be
gone and the stop flag set; otherwise the loop starts hub_rf again inside the reboot
window.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "safe_reboot.sh"

DEVICE_DIRS = "data|tmp|etc|cache"
STUBS = ("killall", "mount", "sleep", "sync")


def alive(pid: int) -> bool:
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    return stat.rsplit(")", 1)[1].split()[0] != "Z"


class SafeReboot(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.root = base / "root"
        self.stub = base / "stub"
        self.stub.mkdir()
        (self.root / "tmp").mkdir(parents=True)
        for name in STUBS:
            p = self.stub / name
            p.write_text("#!/bin/sh\nexit 0\n")
            p.chmod(0o755)
        self.at_reboot = base / "at_reboot"
        reboot = self.stub / "reboot"
        reboot.write_text(
            "#!/bin/sh\n"
            f"ls '{self.root}/tmp' > '{self.at_reboot}'\n"
            "exit 0\n"
        )
        reboot.chmod(0o755)
        self.pids: list[int] = []

    def tearDown(self):
        for pid in self.pids:
            try:
                os.kill(pid, 9)
            except OSError:
                pass
        self.tmp.cleanup()

    def start_supervisor(self) -> int:
        """Start a detached loop shaped like the init.sh supervisors and record its pid."""
        tmp = self.root / "tmp"
        loop = (
            "while true; do "
            f"[ -f '{tmp}/harmony_stop' ] && exit 0; "
            f"echo restart >> '{tmp}/restarts'; "
            "/bin/sleep 0.2; "
            "done"
        )
        out = subprocess.run(
            ["sh", "-c", f"sh -c \"{loop}\" >/dev/null 2>&1 & echo $!"],
            capture_output=True, text=True, check=True, timeout=10,
        )
        pid = int(out.stdout.strip())
        self.pids.append(pid)
        with open(tmp / "harmony_supervisors.pid", "a") as f:
            f.write(f"{pid}\n")
        return pid

    def run_script(self) -> None:
        text = SCRIPT.read_text()
        text = re.sub(r"(?<![\w./$-])/(" + DEVICE_DIRS + r")/", lambda m: f"{self.root}/{m.group(1)}/", text)
        script = pathlib.Path(self.tmp.name) / "safe_reboot.sh"
        script.write_text(text)
        env = dict(os.environ, PATH=f"{self.stub}:/usr/bin:/bin")
        subprocess.run(["sh", str(script)], env=env, capture_output=True, text=True, timeout=30)

    def test_supervisors_stopped_before_reboot(self):
        pid = self.start_supervisor()
        time.sleep(0.3)
        self.assertTrue(alive(pid), "stand-in supervisor did not start")
        self.run_script()
        self.assertTrue(self.at_reboot.exists(), "reboot -f was not reached")
        self.assertIn("harmony_stop", self.at_reboot.read_text().split(),
                      "stop flag must be set before reboot -f")
        deadline = time.time() + 2
        while alive(pid) and time.time() < deadline:
            time.sleep(0.05)
        self.assertFalse(alive(pid), "a supervisor loop survived safe_reboot.sh and keeps restarting daemons")
        self.assertFalse((self.root / "tmp" / "harmony_supervisors.pid").exists())

    def test_runs_without_supervisors(self):
        self.run_script()
        self.assertTrue(self.at_reboot.exists(), "reboot -f must still run when no supervisor pid file exists")


if __name__ == "__main__":
    unittest.main(verbosity=2)
