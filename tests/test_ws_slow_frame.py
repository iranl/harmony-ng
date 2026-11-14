#!/usr/bin/env python3
"""
WebSocket frames are read without blocking the main loop: a client that sends one frame a
byte every 2 s must not stop the Hub from accepting HTTP connections or answering other
WebSocket clients. Also: a frame sent in pieces, a frame with the 64-bit length form, and a
frame larger than the server's buffer (dropped, not cut and re-parsed).
"""

import json
import os
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8902
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def health_time(port, timeout=8):
    """Seconds GET /health takes on a fresh connection (None when no 200 within timeout)"""
    t0 = time.time()
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        s.sendall(b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
        data = s.recv(64)
        s.close()
        return time.time() - t0 if b" 200 " in data else None
    except OSError:
        return None


def masked_frame(payload, opcode=0x1, long_form=False, mask=b"\x01\x02\x03\x04"):
    ln = len(payload)
    if long_form:
        hdr = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack("!Q", ln)
    elif ln < 126:
        hdr = bytes([0x80 | opcode, 0x80 | ln])
    else:
        hdr = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack("!H", ln)
    return hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def pong_for(ws, msg_id, timeout):
    msg, _ = ws.wait_for("pong", timeout, where=lambda m: m.get("msgId") == msg_id)
    return msg


def main():
    print("[-] WebSocket frames read without blocking the main loop")
    with HubHarness(port=PORT) as hub:
        other = hub.ws()
        other.recv_json(3)  # snapshot

        # 1. one client trickles a 125-byte frame, one byte every 2 s
        slow = hub.ws()
        slow.recv_json(3)
        frame = masked_frame(json.dumps({"action": "ping", "msgId": 7, "pad": "x" * 90}).encode()[:125].ljust(125, b" "))
        slow.sock.sendall(frame[:6])  # header and mask
        stop = threading.Event()

        def trickle():
            for b in frame[6:]:
                if stop.wait(2.0):
                    return
                try:
                    slow.sock.sendall(bytes([b]))
                except OSError:
                    return

        th = threading.Thread(target=trickle, daemon=True)
        th.start()
        try:
            time.sleep(1)
            times = [health_time(PORT) for _ in range(3)]
            check(all(t is not None and t < 1.0 for t in times),
                  "/health answers at once while a frame trickles in", str(times))
            t0 = time.time()
            other.send_json({"action": "ping", "msgId": 41})
            got = pong_for(other, 41, 5)
            dt = time.time() - t0
            check(got is not None and dt < 1.0, "another WebSocket client is answered at once",
                  "%s after %.2f s" % (got, dt))
        finally:
            stop.set()
            th.join(3)
            slow.close()

        # 2. a frame sent in three pieces is still read as one frame
        ws = hub.ws()
        ws.recv_json(3)
        fr = masked_frame(json.dumps({"action": "ping", "msgId": 42}).encode())
        for part in (fr[:1], fr[1:9], fr[9:]):
            ws.sock.sendall(part)
            time.sleep(0.3)
        check(pong_for(ws, 42, 3) is not None, "a frame sent in pieces is answered")

        # 3. the 64-bit length form
        ws.sock.sendall(masked_frame(json.dumps({"action": "ping", "msgId": 43}).encode(), long_form=True))
        check(pong_for(ws, 43, 3) is not None, "a frame with the 64-bit length form is answered")

        # 4. a frame larger than the 4 KB buffer: the client is dropped, its bytes are not
        #    parsed as further frames
        big = json.dumps({"action": "ping", "msgId": 44, "pad": "y" * 6000}).encode()
        tail = masked_frame(json.dumps({"action": "ping", "msgId": 45}).encode())
        ws.sock.sendall(masked_frame(big) + tail)
        closed = False
        replies = []
        deadline = time.time() + 3
        while time.time() < deadline:
            try:
                fr = ws.recv_frame(deadline - time.time())
            except (ConnectionError, OSError):
                closed = True
                break
            if fr is None:
                break
            if fr[0] == 0x1:
                replies.append(json.loads(fr[1].decode("utf-8", "replace")))
        check(closed, "a frame larger than the buffer drops the client", "replies %s" % replies)
        check(health_time(PORT) is not None, "the Hub still answers afterwards")
        other.close()

    if failed:
        print("[!] test_ws_slow_frame: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_ws_slow_frame: all checks passed")


if __name__ == "__main__":
    main()
