#!/usr/bin/env python3
"""
devices.json and activities.json are cached in hub_daemon as cJSON trees, and up to 8 HTTP
workers plus the main loop use them at once. The trees used to be handed out without the lock:
a save, a delete or a GET running while another request changed the tree read freed memory and
hub_daemon died (SIGSEGV within a second). This test drives readers and writers of both trees in
parallel against an AddressSanitizer build of hub_daemon_test and checks that

  - no AddressSanitizer report is written and the daemon is still up;
  - no save is lost: commands added to one device by parallel requests are all there at the end;
  - the files on disk are valid JSON and match what the API returns.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import BUILD_DIR, MOCK_LIB, HubHarness, build_hub_daemon_test, build_mock_lib  # noqa: E402

PORT = 8941
RUN_S = 4.0
ASAN_BIN = os.path.join(BUILD_DIR, "hub_daemon_test_asan")
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def asan_runtime(binary):
    """libasan has to come first in LD_PRELOAD, before the mock hardware library"""
    out = subprocess.run(["ldd", binary], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "libasan" in line and "=>" in line:
            return line.split("=>")[1].split()[0]
    return ""


def worker(hub, stop, errors, fn):
    while not stop.is_set():
        try:
            fn(hub)
        except Exception as e:  # a refused connection when the daemon died
            errors.append(str(e))
            time.sleep(0.01)


def main():
    print("[-] Cached devices/activities trees: parallel readers and writers")
    build_mock_lib()
    build_hub_daemon_test(extra_cflags=["-O1", "-fno-omit-frame-pointer", "-fsanitize=address"], out=ASAN_BIN)
    asan = asan_runtime(ASAN_BIN)
    log_dir = tempfile.mkdtemp(prefix="cfg_race_asan_")
    env = {
        "LD_PRELOAD": (asan + ":" if asan else "") + MOCK_LIB,
        "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0:log_path=" + os.path.join(log_dir, "asan"),
    }
    counts = {}
    lock = threading.Lock()

    def count(key):
        with lock:
            counts[key] = counts.get(key, 0) + 1

    seq = iter(range(1, 1000000))

    def save_cmd(hub):
        n = next(seq)
        hub.http_post("/api/command-save", {"deviceId": "1", "name": "Race%d" % n, "raw": "0000 006d 0001 0001 0010 0010"})
        hub.http_post("/api/command-delete", {"deviceId": "1", "command": "Race%d" % n})
        count("command save+delete")

    def batch(hub):
        n = next(seq)
        hub.http_post("/api/commands-batch", {"deviceId": "2", "commands": [
            {"name": "Batch%d" % n, "raw": "0000 006d 0001 0001 0010 0010"}]})
        count("commands-batch")

    def device_save(hub):
        hub.http_post("/api/device-save", {"deviceId": "2", "name": "Media Player", "interKeyDelay": 100})
        count("device-save")

    def activity_save(hub):
        hub.http_post("/api/activity-save", {"id": "11", "name": "Media Player"})
        count("activity-save")

    def reads(hub):
        hub.http_get("/api/devices")
        hub.http_get("/api/inventory")
        hub.http_get("/api/activities")
        hub.http_get("/api/device-commands?deviceId=1")
        count("reads")

    def sync(hub):
        hub.http_post("/api/sync", {})
        count("sync")
        time.sleep(0.05)

    with HubHarness(port=PORT, hub_bin=ASAN_BIN, env=env) as hub:
        stop = threading.Event()
        errors = []
        fns = [save_cmd, save_cmd, batch, device_save, activity_save, reads, reads, reads, sync]
        threads = [threading.Thread(target=worker, args=(hub, stop, errors, fn)) for fn in fns]
        for t in threads:
            t.start()
        start = time.time()
        died_after = None
        while time.time() - start < RUN_S:
            if hub.proc.poll() is not None:
                died_after = time.time() - start
                break
            time.sleep(0.05)
        stop.set()
        for t in threads:
            t.join(10)
        print("    requests: %s, connection errors: %d" % (counts, len(errors)))
        alive = hub.proc.poll() is None
        check(alive, "hub_daemon still running after %.0f s of parallel requests" % RUN_S,
              "(exit status %s after %.1f s)" % (hub.proc.returncode, died_after or 0.0))
        reports = [f for f in os.listdir(log_dir) if f.startswith("asan")]
        detail = ""
        if reports:
            with open(os.path.join(log_dir, reports[0])) as f:
                detail = "\n" + "".join(f.readlines()[:12])
        check(not reports, "no AddressSanitizer report", detail)
        check(counts.get("reads", 0) > 20 and counts.get("command save+delete", 0) > 10,
              "both readers and writers ran", str(counts))

        if alive:
            # Lost updates: commands added to one device by parallel requests must all be kept
            # (6 at a time: the daemon answers 503 above 8 HTTP workers; a 503 is retried)
            names = ["Keep%02d" % i for i in range(12)]

            def add(nm):
                for _ in range(20):
                    st, _ = hub.http_post("/api/command-save",
                                          {"deviceId": "1", "name": nm, "raw": "0000 006d 0001 0001 0010 0010"})
                    if st != 503:
                        return
                    time.sleep(0.05)

            for group in (names[:6], names[6:]):
                ts = [threading.Thread(target=add, args=(nm,)) for nm in group]
                for t in ts:
                    t.start()
                for t in ts:
                    t.join(10)
            st, body = hub.http_get("/api/devices")
            dev1 = [d for d in (body.get("devices", []) if isinstance(body, dict) else []) if str(d.get("id")) == "1"]
            have = {c.get("name") for c in (dev1[0].get("commands", []) if dev1 else [])}
            missing = [n for n in names if n not in have]
            check(st == 200 and not missing, "parallel saves to one device keep every command", "missing %s" % missing)

            # The files on disk are whole and match the API
            with open(os.path.join(hub.data_dir, "config", "devices.json")) as f:
                on_disk = json.load(f)
            check(on_disk == body, "devices.json on disk equals GET /api/devices")
            with open(os.path.join(hub.data_dir, "config", "activities.json")) as f:
                acts = json.load(f)
            check(isinstance(acts.get("activities"), list) and len(acts["activities"]) == 2,
                  "activities.json is whole")
    shutil.rmtree(log_dir, ignore_errors=True)

    if failed:
        print("[-] %d check(s) failed" % failed)
        return 1
    print("[+] config cache race: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
