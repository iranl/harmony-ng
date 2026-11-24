#!/usr/bin/env python3
"""
POST /api/rf/restart on the real hub_rf (host build of its sources with their own main(), the
mock library for /data, a unix socket in place of /dev/rfspi that answers the pipe-table queries).

1. A restored slot table moves remotes to other slots. The new hub_rf reads each slot's version
   file, battery cache and settings file (remote_slot<N>_config.json, sent back to the remote with
   every config push) by slot number, so the old hub_rf moves them to the slot the remote's pipe
   now is before it ends; a slot left without a remote loses them. Here two remotes swap
   slots (2 and 4) and one moves to an empty slot (3 to 5).
2. The restart ends the process even when a thread is stuck: here closing the radio never
   returns (tests/rf_close_hang.c). hub_rf arms alarm(5) on the way out; without it the process
   stayed, and init.sh never started a new one.
3. A slow close does not cut the state move short: the close takes 3.5 s of the first 5 s, then
   a file write of the move (slot 4's, after slot 2's) takes 3 s, as a flash write during JFFS2
   garbage collection can. hub_rf arms alarm(5) again after the close, so the move gets its own
   5 s and completes; with one alarm for both, SIGALRM ended the process with the slots half
   moved (slot 2 with slot 4's files, slot 4 never rewritten).

The battery cache lives in the real /tmp (hub_rf_battery_slot<N>.json, as on the Hub): removed
before and after.
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import BUILD_DIR, MOCK_LIB, REPO_ROOT, TESTS_DIR, build_mock_lib  # noqa: E402

PORT = 9611  # hub_rf's API port in this build (8092 on the Hub)
RF_BIN = os.path.join(BUILD_DIR, "hub_rf_restart_test")
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def build():
    srcs = None
    with open(os.path.join(REPO_ROOT, "build", "compile_all.sh")) as f:
        for line in f:
            line = line.strip()
            if line.startswith('RF_SRCS="'):
                srcs = line[len('RF_SRCS="'):-1].split()
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = (["gcc", "-O2", "-w", "-include", "sys/stat.h", "-include", "time.h", "-DRF_API_PORT=%d" % PORT,
            "-Wl,--wrap=rf_spi_close", "-Wl,--wrap=fsync", "-I" + os.path.join(REPO_ROOT, "payload", "source"), "-o", RF_BIN]
           + [os.path.join(REPO_ROOT, s) for s in srcs]
           + [os.path.join(REPO_ROOT, "payload", "source", "cJSON.c"), os.path.join(TESTS_DIR, "rf_close_hang.c"),
              "-lpthread", "-lm"])
    subprocess.check_call(cmd)
    build_mock_lib()


class Radio:
    """The CC2544 behind /dev/rfspi: answers the EquadID query of the pipes in `remotes` as a
    paired remote, sends nothing else (the receive thread of hub_rf waits in read())"""

    def __init__(self, mock_dir, remotes):
        self.remotes = set(remotes)
        self.path = os.path.join(mock_dir, "hub_rf.sock")
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(self.path)
        self.srv.listen(4)
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _serve(self, c):
        buf = b""
        while True:
            try:
                d = c.recv(256)
            except OSError:
                return
            if not d:
                return
            buf += d
            while len(buf) >= 7:
                n = 30 if buf[0] == 0x12 else 7
                if len(buf) < n:
                    break
                pkt, buf = buf[:n], buf[n:]
                if pkt[:4] == bytes([0x10, 0xFF, 0x83, 0xB5]) and 0x20 <= pkt[4] <= 0x25 and pkt[4] - 0x1F in self.remotes:
                    try:
                        c.sendall(bytes([0x11, 0xFF, 0x83, 0xB5, pkt[4], 0, 0, 0x40, 0x21]) + bytes(11))
                    except OSError:
                        return
                    time.sleep(0.02)

    def close(self):
        self.srv.close()


def battery_path(slot):
    return "/tmp/hub_rf_battery_slot%d.json" % slot


def clear_batteries():
    for s in range(1, 7):
        for p in (battery_path(s), battery_path(s) + ".tmp"):
            if os.path.exists(p):
                os.unlink(p)


def start(mock_dir, log, hang_close=False, extra_env=None):
    env = dict(os.environ, LD_PRELOAD=MOCK_LIB, HARMONY_MOCK_DIR=mock_dir)
    for k in list(env):
        if k.startswith("HUB_RF_TEST_"):
            env.pop(k)
    if hang_close:
        env["HUB_RF_TEST_HANG_CLOSE"] = "1"
    env.update(extra_env or {})
    p = subprocess.Popen([RF_BIN], env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    while time.time() - t0 < 15:
        with open(log) as f:
            if "ready and operational" in f.read():
                time.sleep(0.5)  # the pipe-table replies
                return p
        if p.poll() is not None:
            break
        time.sleep(0.1)
    p.kill()
    with open(log) as f:
        raise RuntimeError("hub_rf did not start: " + f.read()[-600:])


def api(method, path):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (PORT, path), data=b"{}" if method == "POST" else None,
                                 method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except OSError as e:
        return -1, str(e)


def wait_exit(p, limit):
    t0 = time.time()
    while time.time() - t0 < limit:
        if p.poll() is not None:
            return time.time() - t0
        time.sleep(0.05)
    return None


def slots(mock_dir):
    st, body = api("GET", "/api/rf/status")
    if st != 200:
        return {}
    return {s.get("slot"): s for s in json.loads(body).get("slots", [])}


def read_json(path):
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.loads(f.read())


EXPECT = {2: 4, 4: 2, 5: 3}  # new slot: the slot its remote had


def setup_swap(md):
    """Remotes on pipes 1 to 4, slots the same; an import then makes slot 2 pipe 4, slot 4 pipe 2
    (a swap), slot 3 pipe 5 (no remote) and slot 5 pipe 3. Returns the mock /data/harmony."""
    data = os.path.join(md, "data", "harmony")
    os.makedirs(os.path.join(data, "config"))
    with open(os.path.join(data, "rf_slot_map.txt"), "w") as f:
        f.write("1 2 3 4 5 6\n")
    now_ms = int(time.monotonic() * 1000) - 1000
    for s in range(1, 5):
        with open(os.path.join(data, "remote_slot%d_version.json" % s), "w") as f:
            json.dump({"slot": s, "version": "2.5.%d" % s, "config_version": 7, "caps": 127,
                       "remote_id": "2020010100000%d-00000%d" % (s, s), "skin": 111}, f)
        if s > 1:
            with open(battery_path(s), "w") as f:
                json.dump({"percent": 11 * s, "charging": False, "voltage_mv": 3900, "reported_at": 0,
                           "mono_ms": now_ms}, f)
        with open(os.path.join(data, "remote_slot%d_config.json" % s), "w") as f:
            json.dump({"slot": s, "brightness": s, "timeout": 10 * s, "language": "l%d" % s}, f)
    # a settings file left in slot 5, which holds no remote (pipe 5): it must not reach slot 3
    with open(os.path.join(data, "remote_slot5_config.json"), "w") as f:
        json.dump({"slot": 5, "brightness": 9, "timeout": 99, "language": "stale"}, f)
    return data


def check_moved(data, pre):
    """The files of every slot after the old hub_rf ended (setup_swap()'s table)"""
    for new, old in EXPECT.items():
        v = read_json(os.path.join(data, "remote_slot%d_version.json" % new)) or {}
        check(v.get("remote_id") == "2020010100000%d-00000%d" % (old, old) and v.get("version") == "2.5.%d" % old,
              pre + "slot %d's version file is the remote of old slot %d" % (new, old), str(v))
        b = read_json(battery_path(new)) or {}
        check(b.get("percent") == 11 * old, pre + "slot %d's battery is that remote's" % new, str(b))
        c = read_json(os.path.join(data, "remote_slot%d_config.json" % new)) or {}
        check(c.get("language") == "l%d" % old and c.get("brightness") == old and c.get("timeout") == 10 * old
              and c.get("slot") == new, pre + "slot %d's settings file is that remote's (slot field %d)" % (new, new), str(c))
    check(read_json(os.path.join(data, "remote_slot3_version.json")) is None,
          pre + "slot 3 (no remote now) has no version file")
    check(not os.path.exists(battery_path(3)), pre + "slot 3 has no battery")
    check(not os.path.exists(os.path.join(data, "remote_slot3_config.json")),
          pre + "slot 3 has no settings file (not slot 5's leftover)")
    c1 = read_json(os.path.join(data, "remote_slot1_config.json")) or {}
    check(c1.get("language") == "l1", pre + "slot 1's settings file unchanged", str(c1))
    v1 = read_json(os.path.join(data, "remote_slot1_version.json")) or {}
    check(v1.get("remote_id") == "20200101000001-000001", pre + "slot 1 (same pipe) unchanged", str(v1))


def main():
    print("[-] hub_rf restart after a restored slot table: per-slot state follows the pipe; the process ends")
    build()
    clear_batteries()
    root = tempfile.mkdtemp(prefix="rf_restart_")
    try:
        # 1. remotes on pipes 1 to 4, slots the same; the import then makes slot 2 pipe 4,
        #    slot 4 pipe 2 (a swap), slot 3 pipe 5 (no remote) and slot 5 pipe 3
        md = os.path.join(root, "m1")
        data = setup_swap(md)
        radio = Radio(md, {1, 2, 3, 4})
        p = start(md, os.path.join(root, "a.log"))
        before = slots(md)
        check(before.get(2, {}).get("remote_id") == "20200101000002-000002", "before: slot 2 is the remote of pipe 2",
              str(before.get(2)))
        with open(os.path.join(data, "rf_slot_map.txt"), "w") as f:
            f.write("1 4 5 2 3 6\n")  # what an import writes
        st, _ = api("POST", "/api/rf/restart")
        took = wait_exit(p, 15)
        check(st == 202 and took is not None, "restart asked (202) and the process ended", "%s %s" % (st, took))
        if took is None:
            p.kill()
            p.wait()

        check_moved(data, "")
        p = start(md, os.path.join(root, "b.log"))
        after = slots(md)
        for new, old in EXPECT.items():
            s = after.get(new, {})
            check(s.get("active") is True and s.get("remote_id") == "2020010100000%d-00000%d" % (old, old)
                  and s.get("sw_version") == "2.5.%d" % old,
                  "after the restart: slot %d shows the remote of pipe %d" % (new, old), str(s)[:200])
        for new, old in EXPECT.items():
            st, body = api("GET", "/api/rf/remote-config?slot=%d" % new)
            try:
                c = json.loads(body) if st == 200 else {}
            except ValueError:
                c = {}
            check(c.get("language") == "l%d" % old, "after the restart: slot %d's settings are that remote's" % new,
                  "%s %s" % (st, body[:120]))
        s3 = after.get(3, {})
        check(not s3.get("active") and not s3.get("remote_id"), "after the restart: slot 3 is empty", str(s3)[:200])
        p.kill()
        p.wait()
        radio.close()
        clear_batteries()

        # 2. the way out with a stuck close
        md = os.path.join(root, "m2")
        os.makedirs(os.path.join(md, "data", "harmony", "config"))
        radio = Radio(md, {1})
        p = start(md, os.path.join(root, "c.log"), hang_close=True)
        st, _ = api("POST", "/api/rf/restart")
        took = wait_exit(p, 12)
        check(st == 202 and took is not None and p.returncode == -signal.SIGALRM,
              "a stuck close still ends the process (SIGALRM)", "ended after %s s, code %s" % (took, p.returncode))
        if took is None:
            p.kill()
            p.wait()
        radio.close()

        # 3. a slow close, then a slow file write in the move (slot 2's version and settings files
        #    are fsync()ed as calls 1 to 4 after the close, slot 4's version file is call 5)
        clear_batteries()
        md = os.path.join(root, "m3")
        data = setup_swap(md)
        radio = Radio(md, {1, 2, 3, 4})
        p = start(md, os.path.join(root, "d.log"), extra_env={
            "HUB_RF_TEST_SLOW_CLOSE_MS": "3500", "HUB_RF_TEST_STALL_FSYNC_AT": "5",
            "HUB_RF_TEST_STALL_FSYNC_MS": "3000", "HUB_RF_TEST_LOG_FSYNC": "1"})
        with open(os.path.join(data, "rf_slot_map.txt"), "w") as f:
            f.write("1 4 5 2 3 6\n")
        st, _ = api("POST", "/api/rf/restart")
        took = wait_exit(p, 15)
        check(st == 202 and took is not None and took >= 3.5 and p.returncode != -signal.SIGALRM,
              "slow close and slow write: the process ended by itself, not by SIGALRM",
              "ended after %s s, code %s" % (took, p.returncode))
        if took is None:
            p.kill()
            p.wait()
        with open(os.path.join(root, "d.log")) as f:
            log = f.read()
        check("[test] fsync 5 after the close" in log, "slow write: the stall was reached (fsync 5 after the close)",
              log[-400:])
        check_moved(data, "slow close: ")
        radio.close()
    finally:
        clear_batteries()
        shutil.rmtree(root, ignore_errors=True)

    if failed:
        print("[-] test_rf_restart_state: %d FAILED" % failed)
        return 1
    print("[+] test_rf_restart_state passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
