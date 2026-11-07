#!/usr/bin/env python3
"""
Server keepalive and dead-client reaping (spec 3.10), against a hub_daemon_test built with
-DWS_PING_INTERVAL_S=2 -DWS_PONG_TIMEOUT_S=1: a client that answers pings stays connected and
keeps receiving events; one that ignores them is closed within the interval plus the timeout.
"""

import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_hub_daemon_test, BUILD_DIR  # noqa: E402

PORT = 8993
KEEPALIVE_BIN = os.path.join(BUILD_DIR, "hub_daemon_test_keepalive")
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def main():
    build_hub_daemon_test(force=not os.path.exists(KEEPALIVE_BIN),
                          extra_cflags=["-DWS_PING_INTERVAL_S=2", "-DWS_PONG_TIMEOUT_S=1"], out=KEEPALIVE_BIN)
    print("[-] 3.10 keepalive (ping after 2 s idle, drop 1 s later)")
    with HubHarness(port=PORT, hub_bin=KEEPALIVE_BIN) as hub:
        good = hub.ws()
        good.recv_json(3)  # snapshot
        # A client that answers pings (recv_json answers them) stays for 10 s and still gets events
        pings = 0
        t0 = time.time()
        while time.time() - t0 < 6:
            fr = good.recv_frame(0.5)
            if fr and fr[0] == 0x9:
                pings += 1
                good.send_frame(0xA, fr[1])
        check(pings >= 2, "server pinged the idle client (%d pings in 6 s)" % pings)
        hub.http_post("/api/button-press", {"button": "Menu", "action": "press", "source": "remote_rf", "slot": 1})
        msg, _ = good.wait_for("button", 3)
        check(msg is not None, "the answering client still receives events", str(msg))

        # A client that never answers: gone within ~3 s (ping at 2 s, timeout 1 s)
        silent = hub.ws()
        silent.recv_json(3)  # snapshot
        t0 = time.time()
        closed_at = None
        while time.time() - t0 < 8:
            # keep answering the good client's pings meanwhile
            fr = good.recv_frame(0.2)
            if fr and fr[0] == 0x9:
                good.send_frame(0xA, fr[1])
            try:
                silent.sock.settimeout(0.3)
                data = silent.sock.recv(4096)
                if not data:
                    closed_at = time.time() - t0
                    break
                # ignore pings on purpose
            except socket.timeout:
                continue
            except (ConnectionError, OSError):
                closed_at = time.time() - t0
                break
        check(closed_at is not None and closed_at <= 5.0, "silent client closed by the server (%.1f s)" % (closed_at or -1))
        # The answering client is unaffected
        hub.http_post("/api/button-press", {"button": "Menu", "action": "release", "source": "remote_rf", "slot": 1})
        msg, _ = good.wait_for("button", 3, lambda m: m.get("action") == "release")
        check(msg is not None, "answering client still connected after the reap", str(msg))
        # A client that sends its own frames (no pong needed) stays too
        chatty = hub.ws()
        chatty.recv_json(3)
        t0 = time.time()
        alive = True
        while time.time() - t0 < 5:
            chatty.send_json({"action": "ping", "msgId": 1})
            fr = chatty.recv_frame(1.0)
            if fr is None:
                continue
        try:
            chatty.send_json({"action": "ping", "msgId": 2})
            msg, _ = chatty.wait_for("pong", 2, lambda m: m.get("msgId") == 2)
            alive = msg is not None
        except (ConnectionError, OSError):
            alive = False
        check(alive, "a client that sends frames is never dropped")
        chatty.close()
        good.close()
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all keepalive checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
