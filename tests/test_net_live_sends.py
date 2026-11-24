#!/usr/bin/env python3
"""
A person's press is live: sent like a hard key of the remote, so a late one is dropped instead of
sent late. A command of an automation is not live: it waits its turn and goes out.

Test daemon with dry run off; local HTTP devices: FAST answers at once, MUTE takes the connection
and never answers (timeout 4 s), so a send to it holds the one send worker for 4 s and everything
queued behind it waits that long (a live send is late after 2 s in the queue).

- POST /api/ir-send with "live": true behind MUTE is dropped ("waited too long"); the same send
  without it is delivered (it used to be delivered in both cases: the field did not exist);
- WebSocket send_command with "live": true: the same;
- a screen button or device page command of a touchscreen remote (hub_rf's real dispatch thread,
  tests/rf_key_client.c, on the key channel) behind MUTE is dropped; it went through a plain
  POST /api/ir-send and was delivered 4 s late. One that does not wait still goes out at once.
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, REPO_ROOT, BUILD_DIR, MOCK_LIB  # noqa: E402

PORT = 9686
FAST_PORT = 9687
MUTE_PORT = 9688
GOT = []   # paths FAST got
CMDS = ["VolUp", "VolDown", "Up", "Down", "Left", "Right", "Menu", "Home"]


class Fast(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        GOT.append(self.path)
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


def build_client():
    out = os.path.join(BUILD_DIR, "rf_key_client")
    src = os.path.join(REPO_ROOT, "payload", "source")
    os.makedirs(BUILD_DIR, exist_ok=True)
    subprocess.run(["gcc", "-Wall", "-O2", "-I" + src, os.path.join(REPO_ROOT, "tests", "rf_key_client.c"),
                    os.path.join(src, "rf_keymap.c"), os.path.join(src, "rf_state.c"), os.path.join(src, "cJSON.c"),
                    "-Wl,--wrap=http_post_local", "-lpthread", "-lm", "-o", out], check=True)
    return out


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + str(detail)))
        if not cond:
            failures.append(name)

    client = build_client()
    fast = ThreadingHTTPServer(("127.0.0.1", FAST_PORT), Fast)
    threading.Thread(target=fast.serve_forever, daemon=True).start()
    muted = mute(MUTE_PORT)
    tmp = tempfile.mkdtemp(prefix="net_live_")
    log = os.path.join(tmp, "hub.log")
    sock = os.path.join(tmp, "keys.sock")
    env = {"HARMONY_TEST_NET_LOOPBACK_PORTS": "%d,%d" % (FAST_PORT, MUTE_PORT)}
    try:
        with HubHarness(port=PORT, log_path=log, env=env, args=["--key-socket", sock]) as hub:
            post = hub.http_post
            check(post("/api/settings", {"dry_run": False})[0] == 200, "dry run off")
            for did, name, port in (("63", "Fast", FAST_PORT), ("64", "Mute", MUTE_PORT)):
                st, r = post("/api/device-save", {"id": did, "name": name, "type": "HTTP", "netHost": "127.0.0.1",
                                                  "netPort": port, "netTimeoutMs": 4000, "isPowerAlwaysOn": True,
                                                  "commands": [{"name": c, "httpPath": "/" + c.lower()} for c in CMDS]})
                check(st == 200, "device %s saved" % name, (st, r))
            for _ in range(50):
                if os.path.exists(sock):
                    break
                time.sleep(0.1)

            def log_since(mark):
                with open(log, errors="replace") as f:
                    return f.read()[mark:]

            def hold_worker(cmd):
                post("/api/ir-send", {"deviceId": "64", "command": cmd}, timeout=5)
                time.sleep(0.1)

            # 1. POST /api/ir-send behind MUTE: live dropped, the same without live delivered
            mark = os.path.getsize(log)
            del GOT[:]
            hold_worker("VolUp")
            st1, _ = post("/api/ir-send", {"deviceId": "63", "command": "Down", "live": True}, timeout=5)
            st2, _ = post("/api/ir-send", {"deviceId": "63", "command": "Up"}, timeout=5)
            check(st1 == 200 and st2 == 200, "both sends queued", (st1, st2))
            time.sleep(5)
            text = log_since(mark)
            check("/up" in GOT, "ir-send without live: delivered after the wait", GOT)
            check("/down" not in GOT, "ir-send with live: not sent late", GOT)
            check("Fast (63) Down dropped: waited too long" in text, "ir-send with live: dropped as late", text[-500:])

            # 2. WebSocket send_command behind MUTE: the same
            mark = os.path.getsize(log)
            del GOT[:]
            ws = hub.ws()
            try:
                hold_worker("VolDown")
                ws.send_json({"action": "send_command", "id": "a", "deviceId": "63", "command": "Left",
                              "live": True})
                ack1 = ws.wait_for("ack", timeout=5, where=lambda m: m.get("id") == "a")[0]
                ws.send_json({"action": "send_command", "id": "b", "deviceId": "63", "command": "Right"})
                ack2 = ws.wait_for("ack", timeout=5, where=lambda m: m.get("id") == "b")[0]
            finally:
                ws.close()
            check(ack1 and ack1.get("ok") and ack2 and ack2.get("ok"), "both WebSocket sends queued", (ack1, ack2))
            time.sleep(5)
            text = log_since(mark)
            check("/right" in GOT, "send_command without live: delivered after the wait", GOT)
            check("/left" not in GOT and "Fast (63) Left dropped: waited too long" in text,
                  "send_command with live: dropped as late", (GOT, text[-500:]))

            # 3. hub_rf: a screen button behind MUTE is dropped; one that does not wait goes out
            cenv = os.environ.copy()
            cenv["HARMONY_MOCK_DIR"] = hub.mock_dir
            cenv["HARMONY_MOCK_ROLE"] = "hub"
            cenv["LD_PRELOAD"] = MOCK_LIB
            mark = os.path.getsize(log)
            del GOT[:]
            hold_worker("Up")
            r = subprocess.run([client, str(PORT), sock, "500"], input="0 device 63 Menu\n", text=True,
                               capture_output=True, env=cenv, timeout=30)
            check(r.returncode == 0, "hub_rf dispatch ran", r.stderr[-300:])
            time.sleep(5)
            text = log_since(mark)
            check("/menu" not in GOT, "screen button behind a slow device: not sent late", GOT)
            check("Fast (63) Menu dropped: waited too long" in text, "screen button behind a slow device: dropped as late",
                  text[-500:])
            del GOT[:]
            t0 = time.time()
            r = subprocess.run([client, str(PORT), sock, "500"], input="0 device 63 Home\n", text=True,
                               capture_output=True, env=cenv, timeout=30)
            for _ in range(30):
                if "/home" in GOT:
                    break
                time.sleep(0.05)
            check("/home" in GOT and time.time() - t0 < 2.5, "screen button with nothing queued: sent at once", GOT)
    finally:
        fast.shutdown()
        fast.server_close()
        muted.close()
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("net live sends: %d FAILED" % len(failures))
        return 1
    print("all net live sends checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
