#!/usr/bin/env python3
"""
Two activity requests at the same moment (orchestrator): only one may start, the other is
queued, and the Hub ends in the activity whose steps it ran.

orch_switch_activity() used to release its lock after the busy check and only then set the
state and build the transition, so two requests on two threads could both pass the check and
both write the same step queue. The window is a few instructions wide; an LD_PRELOAD shim makes
every pthread_mutex_unlock() of the request threads wait 3 ms so a test hits it every time.
hub_daemon_test on port 9102, its own mock root.
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9102
TRIALS = 6
failed = 0

SHIM = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
/* Widen the gap after each unlock outside the main thread (the HTTP workers) */
int pthread_mutex_unlock(pthread_mutex_t *m) {
    static int (*real)(pthread_mutex_t *);
    if (!real) real = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    int r = real(m);
    if (syscall(SYS_gettid) != getpid()) usleep(3000);
    return r;
}
'''


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def build_shim():
    d = tempfile.mkdtemp(prefix="unlock_shim_")
    src = os.path.join(d, "shim.c")
    out = os.path.join(d, "shim.so")
    with open(src, "w") as f:
        f.write(SHIM)
    subprocess.run(["gcc", "-shared", "-fPIC", "-O2", src, "-o", out, "-ldl"], check=True)
    return out


def wait_settled(hub, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        st, r = hub.http_get("/api/activity")
        if st == 200 and isinstance(r, dict) and not r.get("busy") and not r.get("queued") and not r.get("sending"):
            return r
        time.sleep(0.1)
    return None


def main():
    print("[-] two activity requests at the same moment")
    from hub_harness import MOCK_LIB
    shim = build_shim()
    with HubHarness(port=PORT, env={"LD_PRELOAD": "%s:%s" % (MOCK_LIB, shim)}) as hub:
        both_started = 0
        wrong_activity = 0
        for t in range(TRIALS):
            wait_settled(hub)
            hub.http_post("/api/activity", {"activity": "-1"})
            wait_settled(hub)
            answers = {}
            barrier = threading.Barrier(2)

            def go(act):
                barrier.wait()
                answers[act] = hub.http_post("/api/activity", {"activity": act}, timeout=10.0)

            ths = [threading.Thread(target=go, args=(a,)) for a in ("10", "11")]
            for th in ths:
                th.start()
            for th in ths:
                th.join()
            states = [answers[a][1].get("state") if isinstance(answers[a][1], dict) else None for a in ("10", "11")]
            if states.count("starting") == 2:
                both_started += 1
            final = wait_settled(hub)
            # The request answered "starting" ran first; the queued one ran after it, so the Hub
            # ends in the activity of the queued request
            if states.count("starting") == 1 and states.count("queued") == 1 and final:
                expect = "11" if states[0] == "starting" else "10"
                if final.get("activity") != expect:
                    wrong_activity += 1
            print("    trial %d: %s -> %s" % (t + 1, states, final.get("activity") if final else None))
        check(both_started == 0, "never two transitions started at once", "%d of %d trials" % (both_started, TRIALS))
        check(wrong_activity == 0, "the Hub ends in the activity requested last", "%d trials" % wrong_activity)
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] Activity double start: all checks passed")


if __name__ == "__main__":
    main()
