#!/usr/bin/env python3
"""
POST /api/ir-raw (spec 3.11) against hub_daemon_test: the dry-run hw_ir_send_raw records its
arguments (GET /api/test/ir-raw-last, harness only): signed and unsigned timings give the same
encoder call; defaults; validation errors name the field. 409 (I2S busy) needs the real blaster
lock and is part of the Hub verification.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8992
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


NEC = [9000, 4500, 560, 560, 560, 1690, 560, 560, 560, 1690, 560, 39000]
NEC_SIGNED = [v if i % 2 == 0 else -v for i, v in enumerate(NEC)]


def main():
    print("[-] 3.11 POST /api/ir-raw")
    with HubHarness(port=PORT) as hub:
        st, r = hub.http_post("/api/ir-raw", {"frequency": 38000, "timings": NEC, "port": 1, "repeats": 1})
        check(st == 200 and r.get("ok") is True and r.get("count") == len(NEC), "unsigned form accepted", "%s %s" % (st, r))
        check(r.get("duration_ms") == (sum(NEC) + 999) // 1000, "duration_ms = sum of timings", str(r))
        st, a = hub.http_get("/api/test/ir-raw-last")
        st, r2 = hub.http_post("/api/ir-raw", {"frequency": 38000, "timings": NEC_SIGNED, "port": 1, "repeats": 1})
        check(st == 200 and r2.get("ok") is True, "signed form accepted", "%s %s" % (st, r2))
        st, b = hub.http_get("/api/test/ir-raw-last")
        check(a.get("calls") == 1 and b.get("calls") == 2, "one encoder call per request", "%s %s" % (a, b))
        same = {k: a.get(k) for k in ("frequency", "count", "port", "repeats", "sum_us", "first")} == \
               {k: b.get(k) for k in ("frequency", "count", "port", "repeats", "sum_us", "first")}
        check(same, "signed and unsigned forms give the same encoder call", "%s %s" % (a, b))
        check(a.get("frequency") == 38000 and a.get("count") == len(NEC) and a.get("port") == 1 and a.get("repeats") == 1
              and a.get("first") == NEC[:8], "recorded arguments", str(a))

        # Defaults: frequency 38000, port 7, repeats 1
        st, r = hub.http_post("/api/ir-raw", {"timings": [1000, 500]})
        st, c = hub.http_get("/api/test/ir-raw-last")
        check(st == 200 and c.get("frequency") == 38000 and c.get("port") == 7 and c.get("repeats") == 1, "defaults", str(c))
        # Unmodulated, repeats scale the duration
        st, r = hub.http_post("/api/ir-raw", {"frequency": 0, "timings": [1000, 500], "repeats": 3})
        check(st == 200 and r.get("duration_ms") == 5, "frequency 0 allowed, duration with repeats", str(r))

        # Validation
        for body, field in [
            ({"frequency": 10000, "timings": NEC}, "frequency"),
            ({"frequency": 70000, "timings": NEC}, "frequency"),
            ({"frequency": "38k", "timings": NEC}, "frequency"),
            ({"timings": [1000]}, "timings"),
            ({"timings": list(range(1, 2050))}, "timings"),
            ({"timings": [0, 500]}, "timings"),
            ({"timings": [1000, 70000]}, "timings"),
            ({"timings": [1000, -500, -300, 400]}, "timings"),
            ({"timings": [1000, 500, -300, 400]}, "timings"),
            ({"timings": [1000, 500.5]}, "timings"),
            ({"timings": "9000 4500"}, "timings"),
            ({"timings": NEC, "port": 0}, "port"),
            ({"timings": NEC, "port": 8}, "port"),
            ({"timings": NEC, "repeats": 0}, "repeats"),
            ({"timings": NEC, "repeats": 11}, "repeats"),
            ({}, "timings"),
        ]:
            st, r = hub.http_post("/api/ir-raw", body)
            check(st == 400 and isinstance(r, dict) and r.get("ok") is False and field in r.get("error", ""),
                  "400 names %s for %s" % (field, body if len(str(body)) < 60 else field), "%s %s" % (st, r))
        st, r = hub.http_post("/api/ir-raw", None, raw=b"[1,2]")
        check(st == 400, "400 for a non-object body (%s)" % st)
        # Large body (2048 values) is accepted by the HTTP server
        big = [500 + (i % 7) * 100 for i in range(2048)]
        st, r = hub.http_post("/api/ir-raw", {"timings": big})
        check(st == 200 and r.get("count") == 2048, "2048 timings accepted", "%s %s" % (st, str(r)[:80]))
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all ir-raw checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
