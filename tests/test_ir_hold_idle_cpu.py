#!/usr/bin/env python3
"""The held IR key's player sleeps while no key is held: after a held key has ended, the
daemon must use almost no CPU (a player thread that spun kept hub_daemon at 100 %).
"""
import json
import os
import socket
import sys
import tempfile
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
from hub_harness import HubHarness, BUILD_DIR, build_hub_daemon_test  # noqa: E402
import test_ir_hold  # noqa: E402

PORT = 8952
TICKS = os.sysconf("SC_CLK_TCK")
failed = 0


def check(ok, what, detail=""):
    global failed
    print("  %s %s%s" % ("PASS" if ok else "FAIL", what, (" " + detail) if detail and not ok else ""))
    if not ok:
        failed += 1


def cpu_seconds(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / TICKS   # utime + stime, all threads


def key(sock_path, seq, action):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(json.dumps({"seq": seq, "ts": int(time.monotonic() * 1000), "slot": 1, "button": "VolumeUp",
                             "action": action, "deviceId": test_ir_hold.TV, "command": "VolumeUp"}).encode(), sock_path)
    finally:
        s.close()


def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    build_hub_daemon_test()
    with tempfile.TemporaryDirectory(prefix="irhold_cpu_") as tmp:
        sock_path = os.path.join(tmp, "keys.sock")
        with HubHarness(port=PORT, args=["--key-socket", sock_path],
                        extra_config_files={"devices.json": test_ir_hold.devices()}) as hub:
            with open(os.path.join(hub.mock_dir, "i2s_tx_realtime"), "w") as f:
                f.write("1")
            st, _ = hub.http_post("/api/settings", {"dry_run": False})
            check(st == 200 and os.path.exists(sock_path), "daemon up, key channel bound")
            pid = hub.proc.pid
            # a held key: press, two holds, release
            key(sock_path, 1, "press")
            time.sleep(0.3)
            key(sock_path, 2, "hold")
            time.sleep(0.3)
            key(sock_path, 3, "hold")
            time.sleep(0.2)
            key(sock_path, 4, "release")
            time.sleep(1.0)   # the player ends after its frame on air
            c0, t0 = cpu_seconds(pid), time.monotonic()
            time.sleep(2.0)
            used = (cpu_seconds(pid) - c0) / (time.monotonic() - t0)
            check(used < 0.10, "idle after the held key: hub_daemon uses %.0f %% CPU (at most 10 %%)" % (used * 100),
                  "(a thread is spinning)")
    if failed:
        print("[-] test_ir_hold_idle_cpu: %d failed" % failed)
        return 1
    print("[+] test_ir_hold_idle_cpu: the IR hold player sleeps while idle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
