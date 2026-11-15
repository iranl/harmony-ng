#!/usr/bin/env python3
"""payload/scripts/update.sh apply holds its reboot while an activity switch runs (as agreed in #220).

Same host sandbox as test_update_apply.py (paths moved into a temporary root, fake busybox, wget
serving a local package). The fake busybox also answers GET http://127.0.0.1:8089/api/activity
from a list of replies, one per call, and the reboot script records how many replies were read.
Checks: the reboot comes only after the reply that shows no switch (busy, sending and a queued
request all count as a switch); a switch that does not end lets the update reboot after
UPDATE_ACTIVITY_WAIT_SECS with a message; no answer from hub_daemon reboots at once.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_update_apply as base  # noqa: E402

IDLE = '{"activity":"10","state":2,"busy":false,"queued":null,"sending":false}'
BUSY = '{"activity":"10","state":1,"busy":true,"queued":null,"sending":true}'
SENDING = '{"activity":"11","state":2,"busy":false,"queued":null,"sending":true}'
QUEUED = '{"activity":"11","state":1,"busy":false,"queued":"-1","sending":false}'

# Answers the activity query from $UPD_ACT/replies (line n for call n, the last line after that;
# "down" = no answer), everything else goes to the busybox of test_update_apply.py
ACT_BUSYBOX = """#!/bin/sh
case "$*" in
  *127.0.0.1:8089/api/activity*)
    n=$(cat "$UPD_ACT/calls" 2>/dev/null || echo 0)
    n=$((n + 1))
    echo "$n" > "$UPD_ACT/calls"
    line=$(sed -n "${n}p" "$UPD_ACT/replies")
    [ -z "$line" ] && line=$(sed -n '$p' "$UPD_ACT/replies")
    [ "$line" = "down" ] && exit 1
    echo "$line"
    exit 0 ;;
esac
exec "$UPD_ACT/busybox.orig" "$@"
"""


class RebootWaitsForSwitch(base.UpdateApply):
    def setUp(self):
        super().setUp()
        self.act = self.root / "act"
        self.act.mkdir()
        bb = self.root / "cache/bin/busybox"
        (self.act / "busybox.orig").write_text(bb.read_text())
        (self.act / "busybox.orig").chmod(0o755)
        self.write(bb, ACT_BUSYBOX, 0o755)
        # the new busybox of the package must keep answering too (it replaces this one)
        self.new_files["bin/busybox"] = ACT_BUSYBOX
        reboot = f"#!/bin/sh\ncat {self.act}/calls > {self.root}/tmp/rebooted 2>/dev/null || echo 0 > {self.root}/tmp/rebooted\n"
        self.write(self.data / "safe_reboot.sh", reboot, 0o755)
        self.new_files["scripts/safe_reboot.sh"] = reboot

    def run_with(self, replies: list[str], wait_secs: int = 0):
        (self.act / "replies").write_text("\n".join(replies) + "\n")
        env = {
            "PATH": f"{self.root / 'fakebin'}:/usr/bin:/bin",
            "UPD_SERVE": str(self.root / "serve"),
            "UPD_FAIL_CP": "",
            "UPD_DATA_DIR": str(self.root / "data"),
            "UPD_DATA_BUDGET": "",
            "UPD_ACT": str(self.act),
        }
        if wait_secs:
            env["UPDATE_ACTIVITY_WAIT_SECS"] = str(wait_secs)
        self.serve_bundle()
        r = base.subprocess.run(["sh", str(self.script), "apply", base.REPO], capture_output=True, text=True,
                                timeout=60, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("applied successfully", r.stdout)
        for _ in range(50):  # safe_reboot.sh runs in the background
            if (self.root / "tmp" / "rebooted").exists():
                break
            base.subprocess.run(["sleep", "0.1"])
        return r, int((self.root / "tmp" / "rebooted").read_text().strip() or 0)

    def test_reboot_after_the_switch(self):
        r, calls_at_reboot = self.run_with([BUSY, BUSY, SENDING, QUEUED, IDLE])
        self.assertEqual(calls_at_reboot, 5, r.stdout)
        self.assertIn("activity switch is running", r.stdout)
        self.assertIn("finished", r.stdout)

    def test_idle_reboots_at_once(self):
        r, calls_at_reboot = self.run_with([IDLE])
        self.assertEqual(calls_at_reboot, 1, r.stdout)
        self.assertNotIn("activity switch is running", r.stdout)

    def test_switch_that_does_not_end(self):
        r, calls_at_reboot = self.run_with([BUSY], wait_secs=6)
        self.assertEqual(calls_at_reboot, 3, r.stdout)
        self.assertIn("rebooting anyway", r.stdout)

    def test_no_answer_reboots_at_once(self):
        r, calls_at_reboot = self.run_with(["down"])
        self.assertEqual(calls_at_reboot, 1, r.stdout)


def load_tests(loader, tests, pattern):
    """Only this file's tests, not the ones inherited from test_update_apply.py"""
    suite = unittest.TestSuite()
    for name in sorted(n for n in RebootWaitsForSwitch.__dict__ if n.startswith("test_")):
        suite.addTest(RebootWaitsForSwitch(name))
    return suite


if __name__ == "__main__":
    unittest.main()
