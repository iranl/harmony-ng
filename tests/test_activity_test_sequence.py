#!/usr/bin/env python3
"""
POST /api/activity-test-sequence runs its steps on the request's thread (each step waits its delay,
at most 3 s), so the number of steps is limited to what a saved sequence can hold (64):
- 64 steps run and are answered 200 with the executed count
- 65 steps are refused with 400 at once, nothing is run
- the same for a sequence sent as a string (what the web UIs send)
The daemon runs with --dry-run.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8972


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    with HubHarness(port=PORT) as hub:
        steps = [{"type": "Delay", "delay": 1} for _ in range(64)]
        st, r = hub.http_post("/api/activity-test-sequence", {"steps": steps}, timeout=10)
        check(st == 200 and isinstance(r, dict) and r.get("executed") == 64, "64 steps: 200, all executed", "%s %s" % (st, r))

        # 65 steps of 3 s would hold the worker for more than 3 minutes: refused before any step runs
        steps = [{"type": "Delay", "delay": 3000} for _ in range(65)]
        t0 = time.time()
        st, r = hub.http_post("/api/activity-test-sequence", {"steps": steps}, timeout=5)
        took = time.time() - t0
        check(st == 400 and isinstance(r, dict) and r.get("ok") is False and "64" in str(r.get("error")),
              "65 steps: 400 naming the limit", "%s %s" % (st, r))
        check(took < 2.0, "refused at once, no step run", "%.1f s" % took)

        text = "\n".join(["Delay|||3000"] * 65)  # "type|device|command|delay" lines, as the web UIs send
        st, r = hub.http_post("/api/activity-test-sequence", {"steps": text}, timeout=5)
        check(st == 400, "65 steps sent as text lines: 400", "%s %s" % (st, r))

        st, _ = hub.http_get("/health")
        check(st == 200, "the daemon still answers")

    if failures:
        print("activity test sequence: %d FAILED" % len(failures))
        return 1
    print("all activity test sequence checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
