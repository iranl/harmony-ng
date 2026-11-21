#!/usr/bin/env python3
"""payload/scripts/update.sh on the host, with its /data, /cache and /tmp paths moved into a
temporary directory and busybox replaced by a stub whose wget records its arguments and fails.

- a token is sent as one header argument ("Authorization: token <value>"); it used to be split
  into separate words, so wget got the token as an extra URL and every download failed;
- one apply at a time: with the lock held by a running update.sh, a second apply stops before
  it touches anything; a lock left by a run that no longer exists, or whose pid now belongs
  to another process (other start time), is taken over and removed at the end;
- a failed apply records state=failed with the uptime, and an apply killed with -9 (no EXIT
  trap) still removes the flag and records failed with a last "[-]" line.
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
            f'if [ "$1" = wget ]; then [ -z "$UPD_WGET_SLEEP" ] || sleep "$UPD_WGET_SLEEP"; shift; for a in "$@"; do echo "[$a]"; done >> \'{self.wget_log}\'; '
            f"echo END >> '{self.wget_log}'; exit 1; fi\n"
            'exec "$@"\n')
        stub.chmod(0o755)
        (self.root / "uptime").write_text("1234.56 99.0\n")
        self.env = dict(os.environ, PATH=f"{self.root}/stub:/usr/bin:/bin", UPDATE_UPTIME_FILE=str(self.root / "uptime"))
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
        self.assertFalse((self.root / "tmp" / "hub_update.state").exists(),
                         "a refused run leaves the state of the running one alone")
        self.assertTrue(self.lock.exists(), "the running update keeps its lock")

    def test_failed_apply_records_failed_state(self):
        # wget always fails: no signed manifest, nothing installed
        res = self.run_update("apply")
        self.assertIn("Update refused", res.stdout)
        self.assertEqual((self.root / "tmp" / "hub_update.state").read_text(), "state=failed\nsince=1234\n")
        self.assertFalse((self.root / "tmp" / "hub_updating.flag").exists())
        self.assertIn("[-] The software update ended with exit code 1.", res.stdout)

    def test_killed_apply_removes_the_flag(self):
        # kill -9 of the ( do_apply ) subshell skips its EXIT trap; the parent cleans up
        env = dict(self.env, UPD_WGET_SLEEP="30")
        out_path = self.root / "apply.out"
        with open(out_path, "w") as out_f:
            proc = subprocess.Popen(["sh", str(self.script), "apply"], env=env, stdout=out_f, text=True)
        flag = self.root / "tmp" / "hub_updating.flag"
        for _ in range(100):
            if flag.exists():
                break
            time.sleep(0.05)
        self.assertTrue(flag.exists())
        time.sleep(0.3)
        kids = self.children_of(proc.pid)
        self.assertTrue(kids, "the apply subshell runs")
        below = [d for k in kids for d in self.descendants_of(k)]
        for pid in kids:
            os.kill(pid, 9)
        proc.wait(timeout=30)
        for pid in below:
            try:
                os.kill(pid, 9)
            except ProcessLookupError:
                pass
        out = out_path.read_text()
        self.assertNotEqual(proc.returncode, 0)
        self.assertFalse(flag.exists(), "the flag of the killed run is gone")
        self.assertFalse((self.root / "tmp" / "ota_update").exists(), "its work directory in RAM is gone")
        self.assertEqual((self.root / "tmp" / "hub_update.state").read_text(), "state=failed\nsince=1234\n")
        self.assertIn("[-] The software update ended with exit code 137.", out)

    def children_of(self, pid: int) -> list[int]:
        return [int(p.name) for p in pathlib.Path("/proc").iterdir() if p.name.isdigit() and self.ppid_of(int(p.name)) == pid]

    def descendants_of(self, pid: int) -> list[int]:
        out = []
        for k in self.children_of(pid):
            out += [k] + self.descendants_of(k)
        return out

    @staticmethod
    def ppid_of(pid: int) -> int:
        try:
            return int(pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()[1])
        except (OSError, IndexError, ValueError):
            return -1

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
