#!/usr/bin/env python3
"""payload/scripts/update.sh apply holds its services restart while an activity switch runs (as agreed in #220).

Same host sandbox as test_update_apply.py (paths moved into a temporary root, fake busybox, wget
serving a local package). The fake busybox also answers GET http://127.0.0.1:8089/api/activity
from a list of replies, one per call, and init.sh records how many replies were read at the restart.
Checks: the restart comes only after two replies in a row that show no switch (busy, sending and a
queued request all count as a switch; one idle reply between two switches is not enough); a switch
that does not end lets the update restart after UPDATE_ACTIVITY_WAIT_SECS with a message; a failed
query is "unknown": with hub_daemon running it keeps waiting until UPDATE_ACTIVITY_FAIL_SECS, with
hub_daemon not running it restarts at once; a cap that is not a number keeps the default; the caps
are real seconds from /proc/uptime (slow queries count), and an idle answer at the cap is not called a running
switch.
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
    case "$line" in slow:*) read u r < "$UPDATE_UPTIME_FILE"; echo "$((${u%%.*} + 3)).00 0.00" > "$UPDATE_UPTIME_FILE"; line="${line#slow:}" ;; esac
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
        restart = f"#!/bin/sh\ncat {self.act}/calls > {self.root}/tmp/restarted 2>/dev/null || echo 0 > {self.root}/tmp/restarted\n"
        self.new_files["scripts/init.sh"] = restart
        # time passes only in the fake sleep and in a slow query: a fake /proc/uptime
        self.uptime = self.root / "uptime"
        self.uptime.write_text("100.00 50.00\n")
        sleep = self.root / "fakebin" / "sleep"
        sleep.write_text('#!/bin/sh\n[ -n "$UPDATE_UPTIME_FILE" ] || exit 0\nread u r < "$UPDATE_UPTIME_FILE"\n'
                         'echo "$((${u%%.*} + ${1%%.*})).00 0.00" > "$UPDATE_UPTIME_FILE"\n')
        sleep.chmod(0o755)
        pidof = self.root / "fakebin" / "pidof"
        pidof.write_text('#!/bin/sh\n[ "$UPD_DAEMON_UP" = 1 ]\n')
        pidof.chmod(0o755)

    def run_with(self, replies: list[str], wait_secs=0, daemon_up: bool = True, fail_secs: int = 0):
        (self.act / "replies").write_text("\n".join(replies) + "\n")
        env = {
            "PATH": f"{self.root / 'fakebin'}:/usr/bin:/bin",
            "UPD_SERVE": str(self.root / "serve"),
            "UPD_FAIL_CP": "",
            "UPD_DATA_DIR": str(self.root / "data"),
            "UPD_DATA_BUDGET": "",
            "UPD_ACT": str(self.act),
            "UPD_DAEMON_UP": "1" if daemon_up else "0",
            "UPDATE_UPTIME_FILE": str(self.uptime),
        }
        if wait_secs:
            env["UPDATE_ACTIVITY_WAIT_SECS"] = str(wait_secs)
        if fail_secs:
            env["UPDATE_ACTIVITY_FAIL_SECS"] = str(fail_secs)
        self.serve_bundle()
        r = base.subprocess.run(["sh", str(self.script), "apply", base.REPO], capture_output=True, text=True,
                                timeout=60, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("applied successfully", r.stdout)
        for _ in range(50):  # init.sh restart runs in the background
            if (self.root / "tmp" / "restarted").exists():
                break
            base.subprocess.run(["sleep", "0.1"])
        return r, int((self.root / "tmp" / "restarted").read_text().strip() or 0)

    def test_reboot_after_the_switch(self):
        r, calls_at_reboot = self.run_with([BUSY, BUSY, SENDING, QUEUED, IDLE])
        self.assertEqual(calls_at_reboot, 6, r.stdout)  # two idle replies in a row
        self.assertIn("activity switch is running", r.stdout)
        self.assertIn("finished", r.stdout)

    def test_one_idle_reply_between_switches_is_not_enough(self):
        r, calls_at_reboot = self.run_with([IDLE, BUSY, IDLE, IDLE])
        self.assertEqual(calls_at_reboot, 4, r.stdout)

    def test_idle_reboots_at_once(self):
        r, calls_at_reboot = self.run_with([IDLE])
        self.assertEqual(calls_at_reboot, 2, r.stdout)
        self.assertNotIn("activity switch is running", r.stdout)

    def test_switch_that_does_not_end(self):
        r, calls_at_reboot = self.run_with([BUSY], wait_secs=6)
        self.assertEqual(calls_at_reboot, 3, r.stdout)
        self.assertIn("restarting anyway", r.stdout)

    def test_cap_not_a_number_keeps_the_default(self):
        r, calls_at_reboot = self.run_with([IDLE], wait_secs="abc")
        self.assertEqual(calls_at_reboot, 2, r.stdout)
        self.assertNotIn("anyway", r.stdout)

    def test_cap_is_real_time(self):
        """the query takes 3 s here: the cap of 5 s ends after 1 query and a 2 s sleep, not after 3
        counted sleeps"""
        r, calls_at_reboot = self.run_with(["slow:" + BUSY], wait_secs=5)
        self.assertEqual(calls_at_reboot, 1, r.stdout)
        self.assertIn("still running after 5 s", r.stdout)

    def test_clock_step_does_not_end_the_wait(self):
        """SNTP sets the clock during the wait (the Hub starts on 1970 time): date jumps, uptime not"""
        date = self.root / "fakebin" / "date"
        date.write_text('#!/bin/sh\nn=$(cat "%s/datecalls" 2>/dev/null || echo 0)\necho $((n + 1)) > "%s/datecalls"\n'
                        '[ "$n" -ge 2 ] && echo 1800000000 || echo 1000\n' % (self.root, self.root))
        date.chmod(0o755)
        r, calls_at_reboot = self.run_with([BUSY, BUSY, BUSY, IDLE], wait_secs=30)
        self.assertEqual(calls_at_reboot, 5, r.stdout)
        self.assertNotIn("still running", r.stdout)

    def test_idle_at_the_cap_is_not_a_running_switch(self):
        r, calls_at_reboot = self.run_with([IDLE], wait_secs=1)
        self.assertEqual(calls_at_reboot, 1, r.stdout)
        self.assertNotIn("still running", r.stdout)

    def test_daemon_not_running_reboots_at_once(self):
        r, calls_at_reboot = self.run_with(["down"], daemon_up=False)
        self.assertEqual(calls_at_reboot, 1, r.stdout)

    def test_no_answer_is_unknown(self):
        r, calls_at_reboot = self.run_with(["down", "down", BUSY, IDLE], fail_secs=6)
        self.assertEqual(calls_at_reboot, 5, r.stdout)  # failures did not count as "no switch"
        self.tearDown()
        self.setUp()
        r2, r2_calls = self.run_with(["down"], fail_secs=6)
        self.assertEqual(r2_calls, 4, r2.stdout)
        self.assertIn("has not answered for 6 s", r2.stdout)


def load_tests(loader, tests, pattern):
    """Only this file's tests, not the ones inherited from test_update_apply.py"""
    suite = unittest.TestSuite()
    for name in sorted(n for n in RebootWaitsForSwitch.__dict__ if n.startswith("test_")):
        suite.addTest(RebootWaitsForSwitch(name))
    return suite


if __name__ == "__main__":
    unittest.main()
