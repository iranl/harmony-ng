#!/usr/bin/env python3
"""payload/scripts/bt_reconnect.sh must kill only an earlier bt_reconnect.sh, never a stranger.

The script runs with sh in a sandbox (/tmp and /data moved under a temporary root, no
Bluetooth target configured, so it ends right after its single-instance check). Its pid
file is left holding the pid of another process, as after an instance was killed with -9
and the pid was reused: that process must survive. A pid file of a running earlier
bt_reconnect.sh must still end that instance.
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
SCRIPT = REPO_DIR / "payload" / "scripts" / "bt_reconnect.sh"


def alive(proc: subprocess.Popen) -> bool:
    return proc.poll() is None


class BtReconnectPidFile(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmpdir.name)
        self.root = base / "root"
        (self.root / "tmp").mkdir(parents=True)
        (self.root / "data" / "harmony" / "config").mkdir(parents=True)
        text = re.sub(r"(?<![\w./$-])/(tmp|data)/", lambda m: f"{self.root}/{m.group(1)}/", SCRIPT.read_text())
        self.script = base / "bt_reconnect.sh"
        self.script.write_text(text)
        self.pid_file = self.root / "tmp" / "hub_bt_reconnect.pid"
        self.procs: list[subprocess.Popen] = []

    def tearDown(self):
        for p in self.procs:
            if alive(p):
                p.kill()
            p.wait()
        self.tmpdir.cleanup()

    def spawn(self, cmd: list[str]) -> subprocess.Popen:
        p = subprocess.Popen(cmd)
        self.procs.append(p)
        time.sleep(0.2)
        return p

    def run_script(self) -> None:
        subprocess.run(["sh", str(self.script)], capture_output=True, text=True, timeout=30)

    def test_unrelated_process_survives(self):
        other = self.spawn(["sleep", "30"])
        self.pid_file.write_text(f"{other.pid}\n")
        self.run_script()
        time.sleep(0.2)
        self.assertTrue(alive(other), "bt_reconnect.sh killed an unrelated process whose pid was left in its pid file")

    def test_earlier_instance_is_stopped(self):
        earlier_script = self.script.parent / "earlier" / "bt_reconnect.sh"
        earlier_script.parent.mkdir()
        earlier_script.write_text("#!/bin/sh\nsleep 30\n")
        earlier = self.spawn(["sh", str(earlier_script)])
        self.pid_file.write_text(f"{earlier.pid}\n")
        self.run_script()
        earlier.wait(timeout=5)
        self.assertFalse(alive(earlier))


if __name__ == "__main__":
    unittest.main(verbosity=2)
