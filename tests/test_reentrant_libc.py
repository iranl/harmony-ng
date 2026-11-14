#!/usr/bin/env python3
"""
hub_daemon's request threads and main loop do not use strtok() or inet_ntoa(): both keep their
state in one static buffer, so two threads at once read each other's results (the address of
GET /api/status or the mDNS record, the device ids of an activity save). A shim makes both calls
return wrong results; the daemon's answers must not change:
- GET /api/status reports 127.0.0.1 (the address of the local connection)
- POST /api/activity-save with deviceIds as one string ("1;2") saves both ids
The daemon runs with --dry-run.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, BUILD_DIR, MOCK_LIB, build_mock_lib  # noqa: E402

PORT = 8971
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
SHIM = os.path.join(BUILD_DIR, "libc_static_state_shim.so")


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    build_mock_lib()
    res = subprocess.run(["gcc", "-Wall", "-O2", "-fPIC", "-shared", os.path.join(TESTS_DIR, "libc_static_state_shim.c"), "-o", SHIM],
                         capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        return 1
    with HubHarness(port=PORT, env={"LD_PRELOAD": MOCK_LIB + ":" + SHIM}) as hub:
        st, s = hub.http_get("/api/status")
        check(st == 200 and isinstance(s, dict) and s.get("ip") == "127.0.0.1", "GET /api/status: ip 127.0.0.1",
              "%s %s" % (st, s.get("ip") if isinstance(s, dict) else s))
        st, r = hub.http_post("/api/activity-save", {"name": "Two devices", "deviceIds": "1;2",
                                                     "startSequence": [], "stopSequence": []})
        check(st == 200, "activity saved", "%s %s" % (st, r))
        st, acts = hub.http_get("/api/activities")
        lst = acts if isinstance(acts, list) else (acts or {}).get("activities", []) if isinstance(acts, dict) else []
        saved = [a for a in lst if isinstance(a, dict) and a.get("name") == "Two devices"]
        check(len(saved) == 1 and saved[0].get("deviceIds") == ["1", "2"], "deviceIds \"1;2\" saved as [1, 2]", str(saved))

    if failures:
        print("reentrant libc: %d FAILED" % len(failures))
        return 1
    print("all reentrant libc checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
