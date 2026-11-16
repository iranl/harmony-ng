#!/usr/bin/env python3
"""
HTTP server of hub_daemon (port 8089): replies reach slow clients whole, bodies that did not
arrive whole are refused, request targets must start with '/', status lines and the fixed 403
reply are right, and the static file cache notices a changed file. Runs hub_daemon_test on its
own mock root (port 9100, clients bound to 127.0.0.2).

 1  a large reply reaches a client that starts reading after 3 s
 2  a body shorter than its Content-Length (sender stopped) is answered 400 and not used;
    a negative Content-Length too
 3  "GET .bak/x.txt" (no leading slash) does not read a file next to the web root
 4  429 says "Too Many Requests"
 5  the 403 for the operator role carries its whole JSON body
 6  a changed web UI file is served in its new version without a restart
"""

import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9100
HOST = "127.0.0.2"
failed = 0

USERS = {
    "auth_enabled": True,
    "users": [
        {"username": "admin", "password": "secret123", "role": "admin"},
        {"username": "guest", "password": "guestpass", "role": "user"},
    ],
}


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def connect(rcvbuf=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.bind((HOST, 0))
    s.settimeout(10)
    s.connect((HOST, PORT))
    return s


def read_all(s):
    out = b""
    try:
        while True:
            c = s.recv(65536)
            if not c:
                break
            out += c
    except (socket.timeout, ConnectionResetError):
        pass
    s.close()
    return out


def split_reply(raw):
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    hdrs = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            hdrs[k.strip().lower()] = v.strip()
    return lines[0] if lines else "", hdrs, body


def request(method, path, body=b"", headers=None, rcvbuf=None, wait_before_read=0.0):
    s = connect(rcvbuf)
    h = {"Host": "%s:%d" % (HOST, PORT)}
    if body:
        h["Content-Length"] = str(len(body))
        h["Content-Type"] = "application/json"
    h.update(headers or {})
    s.sendall(("%s %s HTTP/1.1\r\n" % (method, path) + "".join("%s: %s\r\n" % kv for kv in h.items()) +
               "\r\n").encode() + body)
    if wait_before_read:
        time.sleep(wait_before_read)
    return split_reply(read_all(s))


def login(user, pw):
    status, _, body = request("POST", "/api/auth/token", json.dumps({"username": user, "password": pw}).encode())
    try:
        return json.loads(body).get("access_token")
    except ValueError:
        return None


def test_slow_reader():
    print("[-] 1  a large reply reaches a slow client whole")
    layout = json.dumps({"layouts": {"big": "x" * 470000}})
    with HubHarness(port=PORT, host=HOST, extra_data_files={"web_remote_layouts.json": layout}):
        status, hdrs, body = request("GET", "/export/ui-remote-layout", rcvbuf=4096, wait_before_read=3.5)
        want = int(hdrs.get("content-length", "-1"))
        check(status.startswith("HTTP/1.1 200"), "export answered 200", status)
        check(want == len(layout) and len(body) == want, "the whole body arrived",
              "Content-Length %d, got %d bytes" % (want, len(body)))


def test_cut_body():
    print("[-] 2  a body that did not arrive whole is refused")
    with HubHarness(port=PORT, host=HOST) as hub:
        before = hub.http_get("/api/settings")[1]
        s = connect()
        part = b'{"hub_name":"Cut"'
        s.sendall(("POST /api/settings HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\n"
                   "Content-Length: 200\r\n\r\n" % (HOST, PORT)).encode() + part)
        status, _, body = split_reply(read_all(s))
        check(status.startswith("HTTP/1.1 400"), "cut body answered 400", "%s %s" % (status, body[:80]))
        after = hub.http_get("/api/settings")[1]
        check(isinstance(after, dict) and isinstance(before, dict) and
              after.get("hub_name") == before.get("hub_name"), "settings unchanged",
              "%r -> %r" % (before.get("hub_name") if isinstance(before, dict) else before,
                            after.get("hub_name") if isinstance(after, dict) else after))
        status, _, _ = request("POST", "/api/settings", b'{"hub_name":"Neg"}', headers={"Content-Length": "-5"})
        check(status.startswith("HTTP/1.1 400"), "negative Content-Length answered 400", status)
        status, _, body = request("POST", "/api/settings", json.dumps({"hub_name": "Whole"}).encode())
        check(status.startswith("HTTP/1.1 200"), "a whole body is still taken", status)


def test_leading_slash():
    print("[-] 3  request target without a leading slash")
    with HubHarness(port=PORT, host=HOST, extra_root_files={"harmony/www.bak/x.txt": "SIBLING-DIR-CONTENT"}):
        status, _, body = request("GET", ".bak/x.txt")
        check(b"SIBLING" not in body, "file next to the web root not served", "%s %r" % (status, body[:40]))
        check(status.startswith("HTTP/1.1 400"), "answered 400", status)


def test_reason_and_403():
    print("[-] 5  403 body length, 4  429 reason phrase")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS}):
        tok = login("guest", "guestpass")
        check(tok is not None, "operator login")
        status, hdrs, body = request("GET", "/api/users", headers={"Authorization": "Bearer %s" % tok})
        check(status.startswith("HTTP/1.1 403"), "admin route answered 403", status)
        n = int(hdrs.get("content-length", "-1"))
        try:
            parsed = json.loads(body[:n])
        except ValueError:
            parsed = None
        check(n == len(body) and isinstance(parsed, dict) and parsed.get("ok") is False,
              "Content-Length matches the JSON body", "Content-Length %d, body %r" % (n, body))
        status, hdrs429 = "", {}
        for _ in range(8):
            status, hdrs429, _ = request("POST", "/api/auth/token",
                                         json.dumps({"username": "admin", "password": "wrong"}).encode())
            if status.startswith("HTTP/1.1 429"):
                break
        check(status == "HTTP/1.1 429 Too Many Requests", "429 status line", status)
        check(hdrs429.get("retry-after", "").isdigit(), "429 has a Retry-After header", str(hdrs429))


def test_static_cache():
    print("[-] 6  a changed web UI file is served in its new version")
    name = "hng_cache_probe_%d.js" % os.getpid()
    with HubHarness(port=PORT, host=HOST, extra_data_files={}) as hub:
        www = os.path.join(hub.data_dir, "www")
        os.makedirs(www, exist_ok=True)
        path = os.path.join(www, name)
        with open(path, "w") as f:
            f.write("console.log('version 1');")
        _, _, b1 = request("GET", "/" + name)
        time.sleep(1.1)  # a new mtime second
        with open(path, "w") as f:
            f.write("console.log('version 2!');")
        time.sleep(2.2)
        _, _, b2 = request("GET", "/" + name)
        check(b"version 1" in b1, "first request served the file", repr(b1[:40]))
        check(b"version 2" in b2, "changed file served after the check interval", repr(b2[:40]))


def main():
    test_slow_reader()
    test_cut_body()
    test_leading_slash()
    test_reason_and_403()
    test_static_cache()
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] HTTP server replies: all checks passed")


if __name__ == "__main__":
    main()
