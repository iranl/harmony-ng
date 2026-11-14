#!/usr/bin/env python3
"""
WebSocket frames are written without blocking: a client that asks for many snapshots and then
reads only a few kilobytes now and then must not slow down the main loop (HTTP accept, other
WebSocket clients). It is dropped once its output queue is older than a few seconds, while a
normal client keeps getting its answers.
"""

import base64
import json
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8903
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def health_time(port, timeout=8):
    t0 = time.time()
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        s.sendall(b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
        data = s.recv(64)
        s.close()
        return time.time() - t0 if b" 200 " in data else None
    except OSError:
        return None


def masked_text(payload):
    mask = b"\x05\x06\x07\x08"
    ln = len(payload)
    hdr = bytes([0x81, 0x80 | ln]) if ln < 126 else bytes([0x81, 0x80 | 126]) + struct.pack("!H", ln)
    return hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def slow_client(port):
    """A WebSocket client with a 4 KB receive buffer (set before connect)"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.settimeout(5)
    s.connect(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (port, key)).encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(1)
    assert b" 101 " in resp, resp
    return s


def main():
    print("[-] WebSocket writes do not block on a slow reader")
    with HubHarness(port=PORT) as hub:
        other = hub.ws()
        other.recv_json(3)  # snapshot

        slow = slow_client(PORT)
        req = masked_text(json.dumps({"action": "get_snapshot", "msgId": 1}).encode())
        slow.sendall(req * 300)  # about 300 snapshot replies the client will not read in time

        health, pongs = [], []
        closed_at = None
        t0 = time.time()
        next_read = t0 + 1.5
        msg_id = 100
        while time.time() - t0 < 14:
            h = health_time(PORT)
            health.append(h if h is not None else 99.0)
            msg_id += 1
            ts = time.time()
            other.send_json({"action": "ping", "msgId": msg_id})
            m, _ = other.wait_for("pong", 5, where=lambda x, i=msg_id: x.get("msgId") == i)
            pongs.append(time.time() - ts if m else 99.0)
            if closed_at is None and time.time() >= next_read:
                next_read += 1.5
                try:
                    slow.settimeout(0.2)
                    if slow.recv(4096) == b"":
                        closed_at = time.time() - t0
                except socket.timeout:
                    pass
                except OSError:
                    closed_at = time.time() - t0
            time.sleep(0.3)

        slow_count = sum(1 for h in health if h > 0.5)
        check(slow_count == 0, "/health stays fast while the slow client is connected",
              "max %.2f s, %d of %d over 0.5 s" % (max(health), slow_count, len(health)))
        check(max(pongs) < 0.5, "another WebSocket client is answered at once", "max %.2f s" % max(pongs))
        # the server dropped the slow client: its socket ends after the queued bytes
        if closed_at is None:
            try:
                slow.settimeout(2)
                while True:
                    d = slow.recv(65536)
                    if not d:
                        closed_at = time.time() - t0
                        break
            except OSError:
                closed_at = time.time() - t0 if not isinstance(sys.exc_info()[1], socket.timeout) else None
        check(closed_at is not None, "the slow client was dropped", str(closed_at))
        other.send_json({"action": "ping", "msgId": 999})
        m, _ = other.wait_for("pong", 3, where=lambda x: x.get("msgId") == 999)
        check(m is not None, "the normal client is still connected")
        slow.close()
        other.close()

    if failed:
        print("[!] test_ws_slow_reader: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_ws_slow_reader: all checks passed")


if __name__ == "__main__":
    main()
