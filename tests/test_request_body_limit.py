#!/usr/bin/env python3
"""
Request body limits: 64 KB for every route except the whole-map and layout saves (256 KB) and
the config import and the icon upload (2 MB), answered 413 from the headers alone, before the
body is read. And only one import or export runs at a time: a second one gets 409 while the
first is busy.
"""

import json
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8904
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def post_raw(port, path, body_len, send_body=True, timeout=8.0):
    """(status, seconds) of a POST announcing body_len bytes; the body is sent only when
    send_body. status 0 = no answer."""
    t0 = time.time()
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        s.sendall(("POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                   "Content-Length: %d\r\nConnection: close\r\n\r\n" % (path, body_len)).encode())
        if send_body:
            body = (b'{"pad":"' + b"x" * max(0, body_len - 10) + b'"}')[:body_len]
            try:
                s.sendall(body)
            except OSError:
                pass  # the server may refuse and close before the whole body is out
        data = b""
        while b"\r\n" not in data:
            chunk = s.recv(256)
            if not chunk:
                break
            data += chunk
        parts = data.split(b" ", 2)
        return (int(parts[1]) if len(parts) > 1 else 0), time.time() - t0
    except OSError:
        return 0, time.time() - t0
    finally:
        s.close()


def get_status(port, path, rcvbuf=None, read=True):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(10)
    s.connect(("127.0.0.1", port))
    s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n" % path).encode())
    return s


def status_of(sock):
    data = b""
    while b"\r\n" not in data:
        chunk = sock.recv(256)
        if not chunk:
            break
        data += chunk
    parts = data.split(b" ", 2)
    return int(parts[1]) if len(parts) > 1 else 0


def main():
    print("[-] Request body limits and one import/export at a time")
    # a large web remote layout file makes the bundle export big (about 1.5 MB)
    layout = {"big": "z" * 1500000}
    with HubHarness(port=PORT, extra_data_files={"web_remote_layouts.json": json.dumps(layout)}) as hub:
        st, dt = post_raw(PORT, "/api/settings", 70000, send_body=False)
        check(st == 413 and dt < 2, "70 KB announced to /api/settings: 413 before the body",
              "status %d after %.1f s" % (st, dt))
        st, _ = post_raw(PORT, "/api/activity-progress", 65537)
        check(st == 413, "65537-byte body to /api/activity-progress: 413", "status %d" % st)
        st, _ = post_raw(PORT, "/api/activity-progress", 60000)
        check(st not in (0, 413), "60 KB body to /api/activity-progress is still read", "status %d" % st)
        st, _ = post_raw(PORT, "/api/rf-mapping-save?slot=2", 200000)
        check(st not in (0, 413), "200 KB map save to /api/rf-mapping-save is still read", "status %d" % st)
        st, _ = post_raw(PORT, "/api/ui-remote-layout", 200000)
        check(st not in (0, 413), "200 KB layout save to /api/ui-remote-layout is still read", "status %d" % st)
        st, dt = post_raw(PORT, "/api/rf-mapping-save?slot=2", 300000, send_body=False)
        check(st == 413 and dt < 2, "300 KB map save: 413 before the body", "status %d after %.1f s" % (st, dt))
        st, dt = post_raw(PORT, "/api/remote-mapping-save", 300000, send_body=False)
        check(st == 413 and dt < 2, "300 KB BLE map save: 413 before the body", "status %d after %.1f s" % (st, dt))
        st, _ = post_raw(PORT, "/api/activity-save", 70000)
        check(st == 413, "70 KB body to /api/activity-save: 413", "status %d" % st)
        st, _ = post_raw(PORT, "/import", 300000)
        check(st not in (0, 413), "300 KB import is still read (answered by the import route)", "status %d" % st)
        st, _ = post_raw(PORT, "/api/custom-icons", 300000)
        check(st not in (0, 413), "300 KB icon upload is still read", "status %d" % st)
        st, _ = post_raw(PORT, "/api/plugins/install", 300000)
        check(st not in (0, 413), "300 KB plugin archive upload is still read", "status %d" % st)
        st, dt = post_raw(PORT, "/import", 3 * 1024 * 1024, send_body=False)
        check(st == 413 and dt < 2, "3 MB import: 413 before the body", "status %d after %.1f s" % (st, dt))

        # One export at a time: the first one is stuck in send() to a client that does not read
        slow = get_status(PORT, "/export/bundle", rcvbuf=4096)
        time.sleep(0.5)
        second = get_status(PORT, "/export/devices")
        st2 = status_of(second)
        second.close()
        check(st2 == 409, "a second export while the first is running: 409", "status %d" % st2)
        imp_st, _ = post_raw(PORT, "/import", 100)
        check(imp_st == 409, "an import while an export is running: 409", "status %d" % imp_st)
        slow.close()
        time.sleep(3)
        third = get_status(PORT, "/export/devices")
        st3 = status_of(third)
        third.close()
        check(st3 == 200, "the next export after the first ended: 200", "status %d" % st3)
        _ = hub

    if failed:
        print("[!] test_request_body_limit: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_request_body_limit: all checks passed")


if __name__ == "__main__":
    main()
