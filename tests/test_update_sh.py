#!/usr/bin/env python3
"""payload/scripts/update.sh on the host, with its /data, /cache and /tmp paths moved into a
temporary directory and busybox replaced by a stub whose wget records its arguments and fails.

- a token is sent as one header argument ("Authorization: token <value>"); it used to be split
  into separate words, so wget got the token as an extra URL and every download failed;
- one apply at a time: with the lock held by a running update.sh, a second apply stops before
  it touches anything; a lock left by a run that no longer exists, or whose pid now belongs
  to another process (other start time), is taken over and removed at the end.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "update.sh"


class UpdateScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for d in ("tmp", "data/harmony", "cache", "stub"):
            (self.root / d).mkdir(parents=True)
        text = SCRIPT.read_text()
        text = text.replace("/tmp/", f"{self.root}/tmp/")
        text = text.replace("/data/", f"{self.root}/data/")
        text = text.replace("/cache/", f"{self.root}/cache/")
        text = text.replace("/etc/", f"{self.root}/etc/")
        self.script = self.root / "update.sh"
        self.script.write_text(text)
        self.wget_log = self.root / "wget.log"
        stub = self.root / "stub" / "busybox"
        stub.write_text(
            "#!/bin/sh\n"
            f'if [ "$1" = wget ]; then shift; for a in "$@"; do echo "[$a]"; done >> \'{self.wget_log}\'; '
            f"echo END >> '{self.wget_log}'; exit 1; fi\n"
            'exec "$@"\n')
        stub.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{self.root}/stub:/usr/bin:/bin")
        self.lock = self.root / "tmp" / "ota_update.lock"
        self.holder = None

    def tearDown(self):
        if self.holder and self.holder.poll() is None:
            self.holder.kill()
            self.holder.wait()
        self.tmp.cleanup()

    def run_update(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["sh", str(self.script), *args], env=self.env, capture_output=True,
                              text=True, timeout=120)

    def test_token_is_one_header_argument(self):
        self.run_update("check", "https://example.invalid/ota", "tok123")
        args = self.wget_log.read_text().splitlines()
        self.assertIn("[Authorization: token tok123]", args)
        self.assertNotIn("[tok123]", args)
        self.assertNotIn("[Authorization:]", args)

    def test_no_token_no_header(self):
        self.run_update("check", "https://example.invalid/ota")
        self.assertNotIn("[--header]", self.wget_log.read_text().splitlines())

    def test_second_apply_refused_while_one_runs(self):
        self.holder = subprocess.Popen(["sh", "-c", "sleep 30; :", "update.sh"])
        time.sleep(0.2)
        self.lock.mkdir()
        (self.lock / "pid").write_text(f"{self.holder.pid}\n")
        res = self.run_update("apply")
        self.assertIn("Another software update is running", res.stdout)
        self.assertNotIn("Starting software update", res.stdout)
        self.assertFalse((self.root / "tmp" / "hub_updating.flag").exists())
        self.assertTrue(self.lock.exists(), "the running update keeps its lock")

    def start_of(self, pid: int) -> str:
        return pathlib.Path(f"/proc/{pid}/stat").read_text().split(") ")[1].split()[19]

    def test_owner_with_its_start_time_keeps_the_lock(self):
        self.holder = subprocess.Popen(["sh", "-c", "sleep 30; :"])
        time.sleep(0.2)
        self.lock.mkdir()
        (self.lock / "pid").write_text(f"{self.holder.pid}\n")
        (self.lock / "start").write_text(self.start_of(self.holder.pid) + "\n")
        res = self.run_update("apply")
        self.assertIn("Another software update is running", res.stdout)

    def test_pid_reused_by_another_process_taken_over(self):
        # the pid is alive and its command line names update.sh (an editor on the file), but it
        # started at another time than the run that wrote the lock
        self.holder = subprocess.Popen(["sh", "-c", "sleep 30; :", "vi", "update.sh"])
        time.sleep(0.2)
        self.lock.mkdir()
        (self.lock / "pid").write_text(f"{self.holder.pid}\n")
        (self.lock / "start").write_text("1\n")
        res = self.run_update("apply")
        self.assertIn("Starting software update", res.stdout)

    def test_lock_records_the_start_time(self):
        text = self.script.read_text().replace("( do_apply )", "( cat \"$APPLY_LOCK/pid\" \"$APPLY_LOCK/start\" )")
        self.script.write_text(text)
        res = self.run_update("apply")
        pid, start = res.stdout.split()[:2]
        self.assertTrue(pid.isdigit() and start.isdigit(), res.stdout)

    def test_stale_lock_taken_over(self):
        self.lock.mkdir()
        (self.lock / "pid").write_text("999999\n")
        res = self.run_update("apply")
        self.assertIn("Starting software update", res.stdout)
        self.assertFalse(self.lock.exists(), "the lock is removed when the run ends")


if __name__ == "__main__":
    unittest.main()
