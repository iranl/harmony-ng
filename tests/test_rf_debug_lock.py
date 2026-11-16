#!/usr/bin/env python3
"""
The RF debug tools (tools/rf_debug/rf_query.c, rf_sniff.c) refuse to touch /dev/rfspi while
hub_rf runs. hub_rf holds an exclusive flock on its lock file; a second process on /dev/rfspi
can hang the Hub. The tools are built on the host with the lock file moved to a temp dir
(-DHUB_RF_LOCK), the test holds the lock like a running hub_rf, and checks that each tool exits
with 2 and names hub_rf before it opens anything. Without the lock held, the tool goes on to
open /dev/rfspi (absent on the host, so it stops there).
"""

import fcntl
import os
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = ("rf_query", "rf_sniff")


def main():
    tmp = tempfile.mkdtemp(prefix="rf_debug_lock_")
    failed = 0
    try:
        lock = os.path.join(tmp, "hub_rf.lock")
        for t in TOOLS:
            out = os.path.join(tmp, t)
            res = subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror=implicit-function-declaration", "-O2",
                                  '-DHUB_RF_LOCK="%s"' % lock, "-o", out,
                                  os.path.join(REPO_ROOT, "tools", "rf_debug", t + ".c")],
                                 capture_output=True, text=True)
            if res.returncode != 0:
                print(res.stderr)
                print("  FAIL %s does not compile" % t)
                failed += 1
                continue
            with open(lock, "a+") as held:
                fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
                r = subprocess.run([out], capture_output=True, text=True, timeout=10)
                fcntl.flock(held, fcntl.LOCK_UN)
            text = r.stdout + r.stderr
            if r.returncode == 2 and "hub_rf is running" in text and "Opened" not in text:
                print("  PASS %s refuses while hub_rf holds the lock" % t)
            else:
                failed += 1
                print("  FAIL %s ran while hub_rf held the lock (exit %d): %s" % (t, r.returncode, text.strip()[:200]))
            if os.path.exists("/dev/rfspi"):
                continue  # never open the real radio from a test
            r = subprocess.run([out], capture_output=True, text=True, timeout=10)
            if r.returncode != 2 and "/dev/rfspi" in (r.stdout + r.stderr):
                print("  PASS %s goes on to /dev/rfspi when the lock is free" % t)
            else:
                failed += 1
                print("  FAIL %s without the lock held: exit %d %s" % (t, r.returncode, (r.stdout + r.stderr).strip()[:200]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] RF debug tools respect hub_rf's lock")


if __name__ == "__main__":
    main()
