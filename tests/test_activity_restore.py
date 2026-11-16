#!/usr/bin/env python3
"""
At start the orchestrator restores the activity saved in /data/harmony/current_activity. When
that id is no longer a configured activity (deleted, or a restored configuration without it) the
Hub starts as PowerOff instead of reporting a running activity whose power off sends nothing. A
saved activity that exists is still restored as running.
The daemon runs with --dry-run.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8975


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    with HubHarness(port=PORT, extra_data_files={"current_activity": "99\n"}) as hub:
        st, s = hub.http_get("/api/activity")
        check(st == 200 and str(s.get("activity")) == "-1" and s.get("state") == 0,
              "saved activity 99 (not configured): starts as PowerOff, idle", "%s %s" % (st, s))

    with HubHarness(port=PORT, extra_data_files={"current_activity": "10\n"}) as hub:
        st, s = hub.http_get("/api/activity")
        check(st == 200 and str(s.get("activity")) == "10" and s.get("state") == 2,
              "saved activity 10 (configured): restored as running", "%s %s" % (st, s))

    if failures:
        print("activity restore: %d FAILED" % len(failures))
        return 1
    print("all activity restore checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
