#!/usr/bin/env python3
"""
HTTP worker time limits: clients that send their request one byte every two seconds must not
keep the 8 worker slots. Four connections trickle their headers and four trickle a body;
while they hold every slot the Hub answers 503, but the header ones are closed after the
header deadline (10 s) and the body ones after the body deadline (20 s), so /health answers
again although the clients keep sending.
"""

import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8901
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def health_status(port):
    """Status code of GET /health on a fresh connection (0 when no answer)"""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
        s.sendall(b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
        data = s.recv(64)
        s.close()
        parts = data.split(b" ", 2)
        return int(parts[1]) if len(parts) > 1 else 0
    except (OSError, ValueError):
        return 0


class Trickler:
    """One connection that sends `first` at once, then one byte of `rest` every 2 s"""

    def __init__(self, port, first, rest):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.sock.sendall(first)
        self.rest = rest
        self.pos = 0
        self.closed_at = None

    def step(self, now):
        if self.closed_at is not None:
            return
        try:
            self.sock.settimeout(0)
            try:
                self.sock.recv(4096)
                # closed, or answered (what arrived by the deadline) and then closed
                self.closed_at = now
                return
            except BlockingIOError:
                pass
            self.sock.send(self.rest[self.pos % len(self.rest):self.pos % len(self.rest) + 1])
            self.pos += 1
        except OSError:
            self.closed_at = now


def main():
    print("[-] HTTP worker time limits (headers 10 s, body 20 s)")
    with HubHarness(port=PORT) as hub:
        check(health_status(PORT) == 200, "/health answers before the slow clients")

        t0 = time.time()
        hdr = [Trickler(PORT, b"G", b"ET /health HTTP/1.1\r\nX-Slow: " + b"a" * 200) for _ in range(4)]
        body = [Trickler(PORT, b"POST /no-such-route HTTP/1.1\r\nHost: x\r\nContent-Length: 4000\r\n\r\n",
                         b"b" * 4000) for _ in range(4)]
        everyone = hdr + body
        stop = threading.Event()

        def pump():
            while not stop.is_set():
                now = time.time() - t0
                for c in everyone:
                    c.step(now)
                stop.wait(2.0)

        th = threading.Thread(target=pump, daemon=True)
        th.start()
        try:
            time.sleep(3)
            check(health_status(PORT) == 503, "the 8 slow clients hold every worker (503)")
            time.sleep(10.5)  # t = 13.5 s: past the header deadline
            st = health_status(PORT)
            check(st == 200, "/health answers again after the header deadline", "status %d" % st)
            closed = [c.closed_at for c in hdr if c.closed_at is not None]
            check(len(closed) == 4, "the header tricklers were closed", str([c.closed_at for c in hdr]))
            time.sleep(10)  # t = 23.5 s: past the body deadline
            closed = [c.closed_at for c in body if c.closed_at is not None]
            check(len(closed) == 4, "the body tricklers were closed", str([c.closed_at for c in body]))
            check(health_status(PORT) == 200, "/health answers at the end")
        finally:
            stop.set()
            th.join(3)
            for c in everyone:
                try:
                    c.sock.close()
                except OSError:
                    pass
        _ = hub

    if failed:
        print("[!] test_http_worker_timeouts: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_http_worker_timeouts: all checks passed")


if __name__ == "__main__":
    main()
