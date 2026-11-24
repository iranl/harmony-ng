#!/usr/bin/env python3
"""
The send queue of HTTP and TCP devices (payload/source/net_action.c, 16 jobs) when it is full.
Test daemon with dry run off; local devices: FAST answers at once, MUTE takes the connection and
never answers (timeout 4 s; MID is the same server with a 3 s timeout), so it holds the one worker.

- a live key (the key channel of hub_rf) that finds the queue full takes the place of the
  oldest live key that is over 1 s old, and goes out: the measured case, a new key at 2.0 s
  when the oldest queued key is 1.9 s old (before, a key made room only behind one over 2 s
  old, and before that it was refused; the late ones were dropped a moment later);
- POST /api/ir-send answers 503 "send queue full" (it said 500 "IR transmit failed", the text
  of an infrared failure);
- an activity step lost to a full queue is logged once, with the activity and the step.
"""

import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9680
FAST_PORT = 9681
MUTE_PORT = 9682
GOT = []   # (seconds since the case began, path) of every request FAST got
T0 = [time.time()]


class Fast(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        GOT.append((round(time.time() - T0[0], 2), self.path))
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *a):
        pass


def mute(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(32)
    held = []

    def loop():
        while True:
            try:
                c, _ = s.accept()
            except OSError:
                return
            held.append(c)
    threading.Thread(target=loop, daemon=True).start()
    return s


SEQ = [0]


def key(path, device, command, action):
    SEQ[0] += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(json.dumps({"seq": SEQ[0], "ts": int(time.monotonic() * 1000), "slot": 1, "button": command,
                             "action": action, "deviceId": device, "command": command}).encode(), path)
    finally:
        s.close()


def tap(path, device, command):
    key(path, device, command, "press")
    key(path, device, command, "release")


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + str(detail)))
        if not cond:
            failures.append(name)

    fast = ThreadingHTTPServer(("127.0.0.1", FAST_PORT), Fast)
    threading.Thread(target=fast.serve_forever, daemon=True).start()
    muted = mute(MUTE_PORT)
    tmp = tempfile.mkdtemp(prefix="net_queue_")
    log = os.path.join(tmp, "hub.log")
    sock = os.path.join(tmp, "keys.sock")
    env = {"HARMONY_TEST_NET_LOOPBACK_PORTS": "%d,%d" % (FAST_PORT, MUTE_PORT)}
    cmds = ["PowerOn", "InputHdmi1", "VolUp", "VolDown", "Mute", "Menu", "Up", "Down"]
    try:
        with HubHarness(port=PORT, log_path=log, env=env, args=["--key-socket", sock]) as hub:
            post = hub.http_post
            check(post("/api/settings", {"dry_run": False})[0] == 200, "dry run off")
            for did, name, port, tmo in (("63", "Fast", FAST_PORT, 4000), ("64", "Mute", MUTE_PORT, 4000),
                                         ("65", "Mid", MUTE_PORT, 3000)):
                st, r = post("/api/device-save", {"id": did, "name": name, "type": "HTTP", "netHost": "127.0.0.1",
                                                  "netPort": port, "netTimeoutMs": tmo, "isPowerAlwaysOn": True,
                                                  "commands": [{"name": c, "httpPath": "/" + c.lower()} for c in cmds]})
                check(st == 200, "device %s saved" % name, (st, r))
            for _ in range(50):
                if os.path.exists(sock):
                    break
                time.sleep(0.1)

            def log_since(mark):
                with open(log, errors="replace") as f:
                    return f.read()[mark:]

            # 1. Live keys: MID holds the worker for 3 s; 16 FAST keys (0.1 to 0.85 s) fill the
            # queue; at 2.0 s the oldest of them is 1.9 s old, and a new key for FAST takes its
            # place. At 3 s the 15 left are over 2 s old (dropped) and the new one is 1 s old (sent).
            def at(t):
                time.sleep(max(0.0, T0[0] + t - time.time()))
            mark = os.path.getsize(log)
            del GOT[:]
            T0[0] = time.time()
            tap(sock, "65", "VolUp")
            for i in range(16):
                at(0.1 + 0.05 * i)
                tap(sock, "63", cmds[i % 6])
            at(2.0)
            tap(sock, "63", "Down")
            time.sleep(4.5)
            text = log_since(mark)
            check("waited too long in the queue (queue full)" in text, "a late key made room", text[-600:])
            check("Fast (63) Down: send queue full" not in text, "the new key was not refused")
            check(any(p == "/down" for _, p in GOT), "the new key reached the device", GOT)
            check(not any(p != "/down" for _, p in GOT), "the late keys were not sent", GOT)
            time.sleep(1)

            # 2. API taps: two for MUTE (8 s of worker), then FAST taps until the queue is full
            answers = []
            for c in ("VolUp", "VolDown"):
                post("/api/ir-send", {"deviceId": "64", "command": c}, timeout=5)
            time.sleep(0.1)
            for i in range(18):
                st, r = post("/api/ir-send", {"deviceId": "63", "command": cmds[i % 8]}, timeout=5)
                answers.append((st, r.get("error") if isinstance(r, dict) else r))
            refused = [a for a in answers if a[0] != 200]
            check(refused and all(a == (503, "send queue full") for a in refused), "full queue: 503 send queue full",
                  answers)
            time.sleep(9)

            # 3. An activity with more steps than the queue holds behind MUTE (8 s): each lost step logged
            steps = [{"type": "Command", "deviceId": "64", "command": c} for c in ("PowerOn", "InputHdmi1")] + \
                    [{"type": "Command", "deviceId": "63", "command": cmds[i % 8]} for i in range(20)]
            st, r = post("/api/activity-save", {"id": "72", "name": "Many", "type": "Custom",
                                                "startSequence": steps, "stopSequence": []})
            check(st == 200, "activity saved", (st, r))
            mark = os.path.getsize(log)
            post("/api/activity", {"activity": "72"}, timeout=10)
            time.sleep(12)
            lost = [ln for ln in log_since(mark).splitlines() if "[!] Orch: activity '72' step" in ln]
            check(lost and all("lost: the HTTP/TCP send queue is full" in ln for ln in lost),
                  "lost steps logged with the activity", lost)
    finally:
        fast.shutdown()
        fast.server_close()
        muted.close()
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("net queue full: %d FAILED" % len(failures))
        return 1
    print("all net queue full checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
