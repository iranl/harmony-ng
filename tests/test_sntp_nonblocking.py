#!/usr/bin/env python3
"""
SNTP must not stop hub_daemon's main loop. With every NTP query unanswered (a shim sends them
to a local UDP socket that never replies), one round takes about 12 s (six servers, 2 s each).
The round used to run on the main loop, once before it and once on its first pass: no request
was answered for about 20 s after start, and again for 10 s every 20 s while the clock was not
set. Now the daemon answers at once while the round runs on its own thread.
"""

import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, BUILD_DIR, MOCK_LIB, build_hub_daemon_test, build_mock_lib  # noqa: E402

PORT = 8964
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
SHIM = os.path.join(BUILD_DIR, "sntp_silent_shim.so")
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def build_shim():
    os.makedirs(BUILD_DIR, exist_ok=True)
    src = os.path.join(TESTS_DIR, "sntp_silent_shim.c")
    res = subprocess.run(["gcc", "-Wall", "-O2", "-fPIC", "-shared", src, "-ldl", "-o", SHIM],
                         capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        raise RuntimeError("sntp_silent_shim.so compilation failed")


def main():
    print("[-] SNTP off the main loop")
    build_mock_lib()
    build_hub_daemon_test()  # before the clock starts: a rebuild is not start-up time
    build_shim()
    silent = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # receives the queries, never answers
    silent.bind(("127.0.0.1", 0))
    env = {"LD_PRELOAD": MOCK_LIB + ":" + SHIM, "SNTP_SHIM_PORT": str(silent.getsockname()[1])}
    hub = HubHarness(port=PORT, env=env)
    t0 = time.time()
    try:
        hub.start()  # waits up to 5 s for the first /health
    except RuntimeError as e:
        check(False, "daemon answers within 5 s of start while SNTP gets no answer", str(e))
        silent.close()
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    try:
        took = time.time() - t0
        check(took < 5, "daemon answers within 5 s of start while SNTP gets no answer", "(%.1f s)" % took)
        slowest = 0.0
        end = time.time() + 14  # longer than one unanswered round
        while time.time() < end:
            s = time.time()
            st, _ = hub.http_get("/health", timeout=15)
            slowest = max(slowest, time.time() - s)
            if st != 200:
                break
            time.sleep(0.25)
        check(st == 200 and slowest < 1.0, "requests answered during the round (slowest %.2f s)" % slowest)
        silent.settimeout(0.1)
        queries = 0
        try:
            while True:
                silent.recv(512)
                queries += 1
        except socket.timeout:
            pass
        check(queries >= 2, "SNTP queries were sent and left unanswered (%d)" % queries)
    finally:
        hub.stop()
        silent.close()

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] All SNTP checks passed")


if __name__ == "__main__":
    main()
