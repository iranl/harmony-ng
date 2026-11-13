#!/usr/bin/env python3
"""
Authentication and account rules of hub_daemon (port 8089), against hub_daemon_test with
authentication on. Clients bind to 127.0.0.2: loopback for the kernel, not for http_auth.

 1  a WebSocket client gets its token's role before the main loop sees it (operator token,
    frame sent before the 101, 101 held back by an LD_PRELOAD interposer)
 2  Basic auth is under the same failed-login limiter as POST /api/auth/token
 3  the operator role cannot save commands or broadcast progress frames
 4  no account list without an admin, never an admin/admin account
 5.1 names of 32 bytes or more are refused
 5.2 the backup bundle carries the accounts
 5.3 the auth action keeps authentication on without authEnabled; renaming needs the password
 5.4 sessions end with their account (delete, demote, new password)
 5.5 no access tokens while authentication is off
 5.6 /api/mqtt-status does not return the broker password
 5.7 unreadable account files fail closed
 5.11 query parameters are parsed (extra parameters, URL encoding)
"""

import base64
import json
import os
import socket
import struct
import subprocess
import sys
import random
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, MOCK_LIB  # noqa: E402

PORT = 8943
HOST = "127.0.0.2"
failed = 0

USERS = {
    "auth_enabled": True,
    "users": [
        {"username": "admin", "password": "secret123", "role": "admin"},
        {"username": "guest", "password": "guestpass", "role": "user"},
    ],
}

HOLD_101 = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
ssize_t send(int fd, const void *buf, size_t len, int flags) {
    static ssize_t (*real_send)(int, const void *, size_t, int);
    if (!real_send) real_send = (ssize_t (*)(int, const void *, size_t, int))dlsym(RTLD_NEXT, "send");
    if (len >= 12 && memcmp(buf, "HTTP/1.1 101", 12) == 0) usleep(3000);
    return real_send(fd, buf, len, flags);
}
'''


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def call(method, path, body=None, headers=None, raw_body=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST, 0))
    s.settimeout(4)
    s.connect((HOST, PORT))
    h = {"Host": "%s:%d" % (HOST, PORT)}
    data = b""
    if raw_body is not None:
        data = raw_body
    elif body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    if data:
        h["Content-Length"] = str(len(data))
    h.update(headers or {})
    s.sendall(("%s %s HTTP/1.1\r\n" % (method, path) + "".join("%s: %s\r\n" % kv for kv in h.items()) + "\r\n").encode() + data)
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
    try:
        st = int(out.split(b" ", 2)[1])
    except (IndexError, ValueError):
        st = 0
    body_txt = out.partition(b"\r\n\r\n")[2]
    try:
        return st, json.loads(body_txt)
    except ValueError:
        return st, body_txt.decode("utf-8", "replace")


def login(user, pw):
    st, r = call("POST", "/api/auth/token", {"username": user, "password": pw})
    return r.get("access_token") if st == 200 and isinstance(r, dict) else None


def bearer(tok):
    return {"Authorization": "Bearer %s" % tok}


def basic(user, pw):
    return {"Authorization": "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode()).decode()}


def users_file(hub):
    with open(os.path.join(hub.data_dir, "config", "users.json")) as f:
        return json.load(f)


def ws_frame(text):
    payload = text.encode()
    mask = os.urandom(4)
    hdr = bytes([0x81])
    n = len(payload)
    hdr += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n)
    return hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def flood_main_loop(stop):
    """Keep the daemon's main loop iterating: a loopback WebSocket client sending empty pong
    frames (no reply), as a busy Hub would have. Without it a new client waits for the next
    select() and the window is rarely hit on a multi-core host."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", PORT))
    s.sendall(("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n" % PORT).encode())
    s.settimeout(0.2)
    try:
        s.recv(4096)
    except socket.timeout:
        pass
    pong = bytes([0x8A, 0x80]) + b"\0\0\0\0"
    batch = pong * 2048
    while not stop.is_set():
        try:
            s.sendall(batch)
        except OSError:
            break
        try:
            while s.recv(65536):
                pass
        except (socket.timeout, BlockingIOError, OSError):
            pass
    s.close()


def ws_race_attempt(token):
    """Upgrade with an operator token and send {"action":"sync"} before the 101 arrives.
    Returns the ack of the sync, or None."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST, 0))
    s.settimeout(1.5)
    s.connect((HOST, PORT))
    s.sendall(("GET /ws?token=%s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
               % (token, HOST, PORT)).encode())
    time.sleep(random.randint(300, 2500) / 1e6)
    s.sendall(ws_frame(json.dumps({"action": "sync", "msgId": 100})))
    buf = b""
    deadline = time.time() + 1.5
    ack = None
    try:
        while time.time() < deadline and ack is None:
            c = s.recv(65536)
            if not c:
                break
            buf += c
            i = buf.find(b'"action":"sync"')
            if i >= 0:
                j = buf.rfind(b"{", 0, i)
                k = buf.find(b"}", i)
                ack = buf[j:k + 1].decode("utf-8", "replace")
    except socket.timeout:
        pass
    s.close()
    return ack


def test_ws_role_race():
    print("[-] 1  WebSocket role is set before the main loop sees the client")
    tmp = tempfile.mkdtemp(prefix="hold101_")
    src = os.path.join(tmp, "hold.c")
    so = os.path.join(tmp, "hold.so")
    with open(src, "w") as f:
        f.write(HOLD_101)
    subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-o", so, src, "-ldl"], check=True)
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS},
                    env={"LD_PRELOAD": "%s:%s" % (MOCK_LIB, so)}):
        tok = login("guest", "guestpass")
        check(tok is not None, "operator login")
        stop = threading.Event()
        flooder = threading.Thread(target=flood_main_loop, args=(stop,), daemon=True)
        flooder.start()
        time.sleep(0.2)
        oks = 0
        tried = 0
        for _ in range(80):
            ack = ws_race_attempt(tok)
            tried += 1
            if ack and '"ok":true' in ack:
                oks += 1
        stop.set()
        flooder.join(timeout=2)
        check(oks == 0, "operator token never runs the admin action sync (%d of %d)" % (oks, tried))


def test_basic_limit():
    print("[-] 2  Basic auth is rate limited")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS}):
        st, _ = call("GET", "/api/users", headers=basic("admin", "secret123"))
        check(st == 200, "right Basic password accepted", str(st))
        for i in range(3):
            call("GET", "/api/users", headers=basic("admin", "wrong%d" % i))
        st, _ = call("GET", "/api/users", headers=basic("admin", "secret123"))
        check(st == 401, "right password refused while the address is locked", str(st))
        st, r = call("POST", "/api/auth/token", {"username": "admin", "password": "secret123"})
        check(st == 429, "login route locked by the Basic failures", str(st))
        time.sleep(2.2)
        st, _ = call("GET", "/api/users", headers=basic("admin", "secret123"))
        check(st == 200, "accepted again after the lock", str(st))


def test_operator_and_accounts():
    print("[-] 3, 4, 5.x  operator rights, accounts, sessions, tokens")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS},
                    extra_data_files={"mqtt.json": {"enabled": False, "host": "192.0.2.10", "port": 1883,
                                                    "username": "hub", "password": "SAMPLE-MQTT-PASSWORD",
                                                    "baseTopic": "harmony/hub"}}) as hub:
        adm = login("admin", "secret123")
        op = login("guest", "guestpass")
        check(adm and op, "admin and operator logins")
        A, O = bearer(adm), bearer(op)

        st, _ = call("POST", "/api/commands-batch", {"deviceId": "1", "commands": [{"name": "PowerOn", "raw": "X"}]}, O)
        check(st == 403, "3  operator cannot save commands (commands-batch)", str(st))
        st, _ = call("POST", "/api/activity-progress", {"activity": "FORGED"}, O)
        check(st == 403, "3  operator cannot broadcast progress frames", str(st))
        st, _ = call("POST", "/api/bt-mouse", {"target": "x\nPAIR_ON", "dx": 1}, O)
        check(st == 400, "3  bt-mouse refuses a target that is not an address", str(st))
        st, _ = call("GET", "/api/status", headers=O)
        check(st == 200, "3  operator still reads the status", str(st))

        st, r = call("POST", "/api/users", {"username": "admin", "role": "user"}, A)
        check(st == 400, "4  the last admin cannot be demoted", "%s %s" % (st, r))
        names = [(u["username"], u.get("role")) for u in users_file(hub)["users"]]
        check(names.count(("admin", "admin")) == 1 and len(names) == 2, "4  no extra admin account added", str(names))
        st, _ = call("POST", "/api/auth/token", {"username": "admin", "password": "admin"})
        check(st == 401, "4  admin/admin does not log in", str(st))

        long_name = "operator_account_name_with_40_chars_xx"
        st, _ = call("POST", "/api/users", {"username": long_name, "password": "pw123456", "role": "user"}, A)
        check(st == 400, "5.1 name of 32 bytes or more refused", str(st))
        st, _ = call("POST", "/api/system-action", {"action": "auth", "authUsername": long_name,
                                                    "authPassword": "pw123456", "authEnabled": True}, A)
        check(st == 400, "5.1 auth action refuses a long name", str(st))

        st, r = call("GET", "/export/bundle", headers=A)
        check(st == 200 and isinstance(r, dict) and "users" in r, "5.2 backup bundle carries the accounts",
              str(r.keys() if isinstance(r, dict) else r)[:120])

        st, r = call("POST", "/api/system-action", {"action": "auth", "authUsername": "boss"}, A)
        check(st == 400, "5.3 renaming the admin without the password refused", str(st))
        st, r = call("POST", "/api/system-action", {"action": "auth", "authPassword": "secret123"}, A)
        check(st == 200 and isinstance(r, dict) and r.get("auth_enabled") is True,
              "5.3 password change without authEnabled keeps authentication on", str(r))
        st, _ = call("GET", "/api/users")
        check(st == 401, "5.3 still closed to anonymous clients", str(st))
        adm = login("admin", "secret123")  # the password change ended the old session
        A = bearer(adm)

        st, _ = call("POST", "/api/users", {"username": "temp", "password": "temppass", "role": "admin"}, A)
        tmp_tok = login("temp", "temppass")
        st, r = call("POST", "/api/auth/tokens", {"name": "temp token"}, bearer(tmp_tok))
        tmp_pat = r.get("token") if isinstance(r, dict) else None
        check(st == 201 and tmp_pat, "5.4 account temp has a session and a token", str(st))
        st, _ = call("POST", "/api/users/delete", {"username": "temp"}, A)
        check(st == 200, "5.4 admin deletes temp", str(st))
        st, _ = call("GET", "/api/users", headers=bearer(tmp_tok))
        check(st == 401, "5.4 deleted account's session ends", str(st))
        st, _ = call("GET", "/api/users", headers=bearer(tmp_pat))
        check(st == 401, "5.4 deleted account's token ends", str(st))

        st, _ = call("POST", "/api/users", {"username": "boss2", "password": "boss2pass", "role": "admin"}, A)
        b2 = login("boss2", "boss2pass")
        st, _ = call("POST", "/api/users", {"username": "boss2", "role": "user"}, A)
        st, _ = call("GET", "/api/users", headers=bearer(b2))
        check(st in (401, 403), "5.4 demoted admin's session has no admin rights", str(st))
        g2 = login("guest", "guestpass")
        call("POST", "/api/users", {"username": "guest", "password": "newguestpass"}, A)
        st, _ = call("GET", "/api/status", headers=bearer(g2))
        check(st == 401, "5.4 new password ends the old session", str(st))

        st, r = call("GET", "/api/mqtt-status", headers=A)
        cfg = r.get("config", {}) if isinstance(r, dict) else {}
        check("password" not in cfg and cfg.get("has_password") is True, "5.6 no broker password in mqtt-status", str(cfg))

        st, r = call("DELETE", "/api/auth/tokens?id=tok_unknown&_=123", headers=A)
        check(st == 404, "5.11 unknown token id answers 404 (extra query parameter ignored)", str(st))
        call("POST", "/api/users", {"username": "x y", "password": "xypass12", "role": "user"}, A)
        st, r = call("DELETE", "/api/users?username=x%20y&_=1", headers=A)
        check(st == 200, "5.11 URL-encoded name with an extra parameter deleted", "%s %s" % (st, r))


def test_tokens_while_off():
    print("[-] 5.5 no access tokens while authentication is off")
    with HubHarness(port=PORT, host=HOST):
        st, r = call("POST", "/api/auth/tokens", {"name": "planted"})
        check(st == 409, "token creation refused with authentication off", "%s %s" % (st, r))


def test_fail_closed():
    print("[-] 5.7 unreadable account files fail closed")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS}) as hub:
        st, _ = call("GET", "/api/users")
        check(st == 401, "auth on: anonymous refused", str(st))
        open(os.path.join(hub.data_dir, "config", "users.json"), "w").close()
        open(os.path.join(hub.data_dir, "webui_auth.conf"), "w").close()
        st, r = call("GET", "/api/users")
        check(st == 401, "both files empty: anonymous still refused", "%s %s" % (st, r))
        st, r = call("GET", "/api/auth/me")
        check(st == 401 or (isinstance(r, dict) and r.get("authenticated") is False), "auth/me not admin", str(r))


def main():
    test_ws_role_race()
    test_basic_limit()
    test_operator_and_accounts()
    test_tokens_while_off()
    test_fail_closed()
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] all checks passed")


if __name__ == "__main__":
    main()
