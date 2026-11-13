#!/usr/bin/env python3
"""
Guards on the Hub API (port 8089) against requests from the LAN and from web pages:

1. A request whose connection is reset right after it was sent is never run as loopback
   admin (get_client_ip fails closed).
2. GET /api/custom-icons/proxy-fetch passes the URL to wget without a shell, and
   POST /api/bt/reconnect accepts only a Bluetooth address as target: shell syntax in either
   value runs nothing.
3. Requests and WebSocket upgrades from another web site (foreign Origin, or Sec-Fetch-Site
   cross-site without an Origin) are refused; the Hub's own clients (same host, the Android
   app's http://localhost, capacitor://localhost) and clients without these headers pass.

The client binds to 127.0.0.2: the kernel treats it as loopback, http_auth does not.
"""

import json
import os
import socket
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8941
HOST = "127.0.0.2"
failed = 0

USERS = {
    "auth_enabled": True,
    "users": [{"username": "admin", "password": "secret123", "role": "admin"}],
}


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def raw(data, rst=False, timeout=3.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST, 0))
    s.settimeout(timeout)
    s.connect((HOST, PORT))
    s.sendall(data)
    if rst:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()
        return b""
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


def req(method, path, body=None, headers=None, host="127.0.0.2:%d" % PORT):
    h = {"Host": host}
    data = b""
    if body is not None:
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        h.setdefault("Content-Type", "application/json")
        h["Content-Length"] = str(len(data))
    h.update(headers or {})
    head = "%s %s HTTP/1.1\r\n" % (method, path) + "".join("%s: %s\r\n" % kv for kv in h.items()) + "\r\n"
    return head.encode() + data


def status(r):
    try:
        return int(r.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return 0


def users(hub):
    with open(os.path.join(hub.data_dir, "config", "users.json")) as f:
        return [u["username"] for u in json.load(f)["users"]]


def test_reset_bypass():
    print("[-] reset connection is not loopback admin (auth on)")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS}) as hub:
        st = status(raw(req("POST", "/api/users", {"username": "ctl", "password": "pw", "role": "admin"})))
        check(st == 401, "control: unauthenticated POST /api/users is refused", str(st))
        for i in range(40):
            raw(req("POST", "/api/users", {"username": "rst%d" % i, "password": "pw", "role": "admin"}), rst=True)
        time.sleep(0.8)
        created = [u for u in users(hub) if u.startswith("rst")]
        check(created == [], "40 requests reset right after sending created no account", str(created))


def test_shell_routes():
    print("[-] proxy-fetch and bt/reconnect run no shell syntax")
    tmp = tempfile.mkdtemp(prefix="hng_guard_")
    m1 = os.path.join(tmp, "m1")
    m2 = os.path.join(tmp, "m2")
    m3 = os.path.join(tmp, "m3")
    with HubHarness(port=PORT, host=HOST) as hub:  # auth off: every LAN client is admin
        q = "url=http://x/%%27;touch+%s;%%27" % m1
        st = status(raw(req("GET", "/api/custom-icons/proxy-fetch?" + q)))
        check(st in (400, 502), "proxy-fetch with quote and blanks answered %s" % st)
        q = "url=http://x/%%27;touch${IFS}%s;%%27" % m2
        st = status(raw(req("GET", "/api/custom-icons/proxy-fetch?" + q)))
        check(st in (400, 502), "proxy-fetch with quote, no blanks answered %s" % st)
        st = status(raw(req("POST", "/api/bt/reconnect",
                            {"target": "x;touch %s;#" % m3, "duration": 30, "delay": 5})))
        check(st == 400, "bt/reconnect with shell syntax in target refused", str(st))
        st = status(raw(req("POST", "/bt/reconnect", b"target=x%%3Btouch+%s&delay=5" % m3.encode(),
                            headers={"Content-Type": "application/x-www-form-urlencoded"})))
        check(st == 400, "bt/reconnect form body with shell syntax refused", str(st))
        st = status(raw(req("POST", "/api/bt/reconnect", {"target": "00:11:22:33:44:02", "delay": 5})))
        check(st == 200, "bt/reconnect with a Bluetooth address accepted", str(st))
        st = status(raw(req("POST", "/api/bt/reconnect", {"delay": 5})))
        check(st == 200, "bt/reconnect without target accepted", str(st))
        time.sleep(0.8)
        for m in (m1, m2, m3):
            check(not os.path.exists(m), "nothing ran: %s absent" % os.path.basename(m))


def test_cross_site():
    print("[-] requests from other web sites are refused")
    own = "127.0.0.2:%d" % PORT
    with HubHarness(port=PORT, host=HOST) as hub:
        cases = [
            ({}, 200, "no Origin (HA integration, curl)"),
            ({"Origin": "http://" + own}, 200, "Origin = the Hub itself (web UI)"),
            ({"Origin": "http://localhost"}, 200, "Origin http://localhost (Android app)"),
            ({"Origin": "capacitor://localhost"}, 200, "Origin capacitor://localhost"),
            ({"Origin": "http://localhost:5173"}, 200, "Origin local dev server"),
            ({"Sec-Fetch-Site": "same-origin"}, 200, "Sec-Fetch-Site same-origin"),
            ({"Origin": "http://evil.example"}, 403, "foreign Origin"),
            ({"Origin": "null"}, 403, "Origin null"),
            ({"Origin": "http://" + own + ".evil.example"}, 403, "Origin with the Hub's host as prefix"),
            ({"Sec-Fetch-Site": "cross-site"}, 403, "no Origin, Sec-Fetch-Site cross-site (img, form)"),
            ({"Sec-Fetch-Site": "same-site"}, 403, "no Origin, Sec-Fetch-Site same-site"),
            ({"Origin": "https://proxy.example", "X-Forwarded-Host": "proxy.example"}, 200,
             "behind a reverse proxy (X-Forwarded-Host)"),
        ]
        for hdrs, want, name in cases:
            st = status(raw(req("GET", "/api/status", headers=hdrs)))
            check(st == want, "%s -> %d" % (name, want), "got %s" % st)
        st = status(raw(req("POST", "/api/bt/reconnect", b"delay=5",
                            headers={"Origin": "http://evil.example",
                                     "Content-Type": "application/x-www-form-urlencoded"})))
        check(st == 403, "form post from another site refused", str(st))
        r = raw(req("OPTIONS", "/api/settings", headers={"Origin": "http://evil.example",
                                                          "Access-Control-Request-Method": "POST"}))
        check(status(r) == 403 and b"Access-Control-Allow-Origin" not in r, "preflight from another site refused",
              r[:80])
        r = raw(req("OPTIONS", "/api/settings", headers={"Origin": "http://localhost",
                                                          "Access-Control-Request-Method": "POST"}))
        check(status(r) == 204 and b"Access-Control-Allow-Origin: http://localhost" in r,
              "preflight from the Android app allowed", r[:120])
        ws_hdr = {"Upgrade": "websocket", "Connection": "Upgrade", "Sec-WebSocket-Version": "13",
                  "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ=="}
        r = raw(req("GET", "/ws", headers=dict(ws_hdr, Origin="http://evil.example")), timeout=1.5)
        check(status(r) != 101, "WebSocket from another site refused", r[:60])
        r = raw(req("GET", "/ws", headers=dict(ws_hdr, Origin="http://" + own)), timeout=1.5)
        check(status(r) == 101, "WebSocket from the web UI accepted", r[:60])


def main():
    test_reset_bypass()
    test_shell_routes()
    test_cross_site()
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] all checks passed")


if __name__ == "__main__":
    main()
