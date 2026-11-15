#!/usr/bin/env python3
"""
Access tokens for the app's login (hub_daemon, port 8089), against hub_daemon_test with
authentication on. Clients bind to 127.0.0.2: loopback for the kernel, not for http_auth.

The Android app trades the password for a personal access token once and keeps only the token,
so an operator (role user) needs to create and delete its own tokens:

 1  the app's sequence: log in, create a token, end the session; the token keeps working
 2  an operator creates a token for its own account (session or Basic); it is always role user
 3  an operator cannot create a token while calling with a token
 4  an operator still cannot list tokens
 5  an operator cannot delete another account's token (404), and the token keeps working
 6  an operator deletes the token it calls with (the app's log out), and of its own account
 7  an admin still deletes any token
"""

import base64
import json
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9300
HOST = "127.0.0.2"
failed = 0

USERS = {
    "auth_enabled": True,
    "users": [
        {"username": "admin", "password": "secret123", "role": "admin"},
        {"username": "guest", "password": "guestpass", "role": "user"},
        {"username": "other", "password": "otherpass", "role": "user"},
    ],
}


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def call(method, path, body=None, headers=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST, 0))
    s.settimeout(4)
    s.connect((HOST, PORT))
    h = {"Host": "%s:%d" % (HOST, PORT)}
    data = b""
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
        h["Content-Length"] = str(len(data))
    h.update(headers or {})
    s.sendall(("%s %s HTTP/1.1\r\n" % (method, path) + "".join("%s: %s\r\n" % kv for kv in h.items())
               + "\r\n").encode() + data)
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
    try:
        return st, json.loads(out.partition(b"\r\n\r\n")[2])
    except ValueError:
        return st, None


def login(user, pw):
    st, r = call("POST", "/api/auth/token", {"username": user, "password": pw})
    return r.get("access_token") if st == 200 and isinstance(r, dict) else None


def bearer(tok):
    return {"Authorization": "Bearer %s" % tok}


def basic(user, pw):
    return {"Authorization": "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode()).decode()}


def make_token(headers, name="Android app", role="admin"):
    st, r = call("POST", "/api/auth/tokens", {"name": name, "role": role}, headers)
    r = r if isinstance(r, dict) else {}
    return st, r


def main():
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS}):
        adm = login("admin", "secret123")
        check(adm is not None, "admin logs in")
        A = bearer(adm)

        print("[-] 1  the app's sequence")
        sess = login("guest", "guestpass")
        st, r = make_token(bearer(sess))
        app_tok, app_id = r.get("token"), r.get("id")
        check(st == 201 and app_tok and app_tok.startswith("pat_"), "1  operator session creates a token", "%s %s" % (st, r))
        st, _ = call("POST", "/api/auth/logout", headers=bearer(sess))
        st, _ = call("GET", "/api/status", headers=bearer(sess))
        check(st == 401, "1  the session ends at logout", str(st))
        st, _ = call("GET", "/api/status", headers=bearer(app_tok))
        check(st == 200, "1  the token keeps working after the session ended", str(st))
        st, _ = call("POST", "/api/activity", {"activity": "-1"}, bearer(app_tok))
        check(st != 401 and st != 403, "1  the token may control the Hub", str(st))

        print("[-] 2  operator tokens are role user")
        check(r.get("role") == "user", "2  role admin asked by an operator gives user", str(r))
        st, _ = call("GET", "/api/users", headers=bearer(app_tok))
        check(st == 403, "2  the operator token has no admin rights", str(st))
        st, r2 = make_token(basic("guest", "guestpass"), name="Second phone")
        guest_basic_id = r2.get("id")
        check(st == 201 and r2.get("role") == "user", "2  Basic login creates a token too", "%s %s" % (st, r2))

        print("[-] 3  no token from a token")
        st, r3 = make_token(bearer(app_tok), name="Planted")
        check(st == 403 and not r3.get("token"), "3  operator calling with a token cannot create one", "%s %s" % (st, r3))

        print("[-] 4  listing stays admin only")
        st, _ = call("GET", "/api/auth/tokens", headers=bearer(app_tok))
        check(st == 403, "4  operator cannot list tokens", str(st))

        print("[-] 5  no deleting another account's token")
        st, ra = make_token(A, name="Home Assistant")
        adm_pat, adm_id = ra.get("token"), ra.get("id")
        st, ro = make_token(basic("other", "otherpass"), name="Android app")
        other_pat, other_id = ro.get("token"), ro.get("id")
        st, _ = call("DELETE", "/api/auth/tokens?id=%s" % adm_id, headers=bearer(app_tok))
        check(st == 404, "5  operator cannot delete the admin's token", str(st))
        st, _ = call("DELETE", "/api/auth/tokens?id=%s" % other_id, headers=bearer(app_tok))
        check(st == 404, "5  operator cannot delete another operator's token", str(st))
        st, _ = call("DELETE", "/api/auth/tokens?id=%s" % other_id, headers=basic("guest", "guestpass"))
        check(st == 404, "5  not with Basic either", str(st))
        st, _ = call("GET", "/api/status", headers=bearer(adm_pat))
        st2, _ = call("GET", "/api/status", headers=bearer(other_pat))
        check(st == 200 and st2 == 200, "5  both tokens still work", "%s %s" % (st, st2))

        print("[-] 6  operator deletes its own tokens")
        st, _ = call("DELETE", "/api/auth/tokens?id=%s" % app_id, headers=bearer(app_tok))
        check(st == 200, "6  the token deletes itself (log out)", str(st))
        st, _ = call("GET", "/api/status", headers=bearer(app_tok))
        check(st == 401, "6  the deleted token is refused", str(st))
        sess = login("guest", "guestpass")
        st, _ = call("DELETE", "/api/auth/tokens?id=%s" % guest_basic_id, headers=bearer(sess))
        check(st == 200, "6  the account's session deletes the account's token", str(st))

        print("[-] 7  admin deletes any token")
        st, _ = call("DELETE", "/api/auth/tokens?id=%s" % other_id, headers=A)
        check(st == 200, "7  admin deletes an operator's token", str(st))
        st, _ = call("GET", "/api/status", headers=bearer(other_pat))
        check(st == 401, "7  that token is refused", str(st))

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] all checks passed")


if __name__ == "__main__":
    main()
