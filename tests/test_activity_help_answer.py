#!/usr/bin/env python3
"""
A Help answer ("device X is off": send its power toggle) only counts while the Hub is still in
the activity of that Help. After a power off or a switch to another activity the open questions
are void: the answer is refused with 409 and nothing is sent (it used to send the toggle, which
switched the device on while everything else was off). An answer while the Hub is in the Help's
activity still sends the toggle.
The daemon runs with --dry-run; sends are counted in its log.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8976


def state_of(hub):
    st, body = hub.http_get("/api/activity")
    return body if isinstance(body, dict) else {}


def wait_state(hub, activity, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        s = state_of(hub)
        if str(s.get("activity")) == activity and s.get("state") in (0, 2) and not s.get("busy"):
            return True
        time.sleep(0.05)
    return False


def toggles_sent(log_path):
    return open(log_path, errors="replace").read().count("IR Keycode simulated: ") if os.path.exists(log_path) else 0


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    log_fd, log_path = tempfile.mkstemp(prefix="help_answer_", suffix=".log")
    os.close(log_fd)
    try:
        with HubHarness(port=PORT, log_path=log_path) as hub:
            st, _ = hub.http_post("/api/activity", {"activity": "11"})
            check(st == 200 and wait_state(hub, "11"), "activity 11 running")
            time.sleep(0.5)
            st, r = hub.http_post("/api/activity/help", {"activity": "11"})
            ids = [d.get("id") for d in (r.get("toggleDevices") or [])] if isinstance(r, dict) else []
            check(st == 200 and "2" in ids, "Help in 11 asks about device 2 (power toggle)", "%s %s" % (st, r))
            check(wait_state(hub, "11"), "the Help re-send ends")
            time.sleep(0.5)

            st, _ = hub.http_post("/api/poweroff", {})
            check(st == 200 and wait_state(hub, "-1"), "power off")
            time.sleep(0.5)
            before = toggles_sent(log_path)
            st, r = hub.http_post("/api/activity/help/answer", {"device": "2", "on": False})
            time.sleep(0.3)
            check(st == 409 and toggles_sent(log_path) == before,
                  "answer 'device 2 is off' after the power off: 409, nothing sent",
                  "%s %s, sends %d -> %d" % (st, r, before, toggles_sent(log_path)))

            st, _ = hub.http_post("/api/activity", {"activity": "11"})
            check(st == 200 and wait_state(hub, "11"), "activity 11 running again")
            time.sleep(0.5)
            st, r = hub.http_post("/api/activity/help/answer", {"device": "2", "on": False})
            check(st in (404, 409), "the old question stays void after coming back to 11", "%s %s" % (st, r))

            st, r = hub.http_post("/api/activity/help", {"activity": "11"})
            check(st == 200 and wait_state(hub, "11"), "a new Help in 11")
            time.sleep(0.5)
            before = toggles_sent(log_path)
            st, r = hub.http_post("/api/activity/help/answer", {"device": "2", "on": False})
            time.sleep(0.3)
            check(st == 200 and r.get("sent") is True and toggles_sent(log_path) == before + 1,
                  "answer while still in 11: 200, the toggle is sent", "%s %s" % (st, r))
    finally:
        os.unlink(log_path)

    if failures:
        print("activity help answer: %d FAILED" % len(failures))
        return 1
    print("all activity help answer checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
