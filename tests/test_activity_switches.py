#!/usr/bin/env python3
"""
The daemon counts the activity switches it begins in /tmp/activity_switches (orchestrator.c).

hub_rf keeps a remote's key layer (rf_keymap.h) only while that count stands, so every switch
must add one. Asking again for the activity that runs (a power off while off too) is no switch:
nothing is sent, and a client that asks again (an automation, a second tap) must not turn a
layer off.
A daemon that starts goes on from the count in the file: hub_rf may outlive it, and a count
that began again at 1 could come back to the one a layer was turned on at.
The daemon runs with --dry-run.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9273
COUNT_FILE = "/tmp/activity_switches"


def count():
    try:
        return int(open(COUNT_FILE).read().strip() or "0")
    except (OSError, ValueError):
        return 0


def wait_idle(hub, activity, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        st, s = hub.http_get("/api/activity")
        if isinstance(s, dict) and str(s.get("activity")) == activity and not s.get("busy"):
            return True
        time.sleep(0.1)
    return False


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    log_dir = tempfile.mkdtemp(prefix="act_switches_")
    with open(COUNT_FILE, "w") as f:
        f.write("40\n")  # what an earlier daemon left
    with HubHarness(port=PORT, log_path=os.path.join(log_dir, "hub1.log")) as hub:
        print("[-] every switch adds one")
        check(count() == 40, "starting the daemon is no switch", str(count()))
        st, r = hub.http_post("/api/activity", {"activity": "10"})
        check(st == 200 and wait_idle(hub, "10"), "activity 10 started", "%s %s" % (st, r))
        check(count() == 41, "a start: one more, on from the count in the file", str(count()))
        st, r = hub.http_post("/api/activity", {"activity": "10"})
        check(st == 200 and wait_idle(hub, "10"), "activity 10 started again", "%s %s" % (st, r))
        check(count() == 41, "the same activity started again: no switch", str(count()))
        st, r = hub.http_post("/api/poweroff", {})
        check(st == 200 and wait_idle(hub, "-1"), "power off", "%s %s" % (st, r))
        check(count() == 42, "a power off: one more", str(count()))
        st, r = hub.http_post("/api/poweroff", {})
        check(st == 200 and wait_idle(hub, "-1"), "power off while off", "%s %s" % (st, r))
        check(count() == 42, "a power off while off: no switch", str(count()))
    with HubHarness(port=PORT, log_path=os.path.join(log_dir, "hub2.log")) as hub:
        print("[-] a restarted daemon goes on from the file")
        st, r = hub.http_post("/api/activity", {"activity": "10"})
        check(st == 200 and wait_idle(hub, "10"), "activity 10 started", "%s %s" % (st, r))
        check(count() == 43, "one more than the daemon before it left", str(count()))
        hub.http_post("/api/poweroff", {})
        wait_idle(hub, "-1")

    if failures:
        print("[!] %d check(s) failed: %s" % (len(failures), ", ".join(failures)))
        sys.exit(1)
    print("[+] Activity switch count: all checks passed")


if __name__ == "__main__":
    main()
