#!/usr/bin/env python3
"""
Access tokens for the app's login (hub_daemon, port 8089), against hub_daemon_test with
authentication on. Clients bind to 127.0.0.2: loopback for the kernel, not for http_auth.

The Android app trades the password for a personal access token once and keeps only the token,
so an operator (role user) needs to create and delete its own tokens:

 1  the app's sequence: log in, create a token, end the session; the token keeps working
 2  an operator creates a token for its own account (session or Basic); it is always role user
 3  an operator cannot create a token while calling with a token
 4  an operator lists only its own account's tokens
 5  an operator cannot delete another account's token (404), and the token keeps working
 6  an operator deletes the token it calls with (the app's log out), and of its own account
 7  an admin still deletes any token
 8  the list shows each token's owner
 9  a token made with a token belongs to the same account and ends with it
10  a token without a name cannot make a token for an operator (decided by the credential)
11  limits: 10 tokens per operator account, 64 in all; other accounts' tokens keep working
12  a token file that cannot be read or parsed is left alone (500), never rebuilt
13  creates and deletes at the same time lose nothing (the file is changed under one lock)
14  a new password ends the account's sessions, not its tokens (delete the token to lock a phone out)
15  turning authentication on with a token file that cannot be read: the reply says the tokens are
    not listed, the file is left alone
16  at most 30 token changes per operator account and minute (429)
17  an operator lists the tokens of its own account (login or token), and deletes them with its token
18  an unreadable file: the 500 says what to do; POST /api/auth/tokens-reset (admin with an account
    login, {"confirm": true}) moves it to tokens.json.bad and starts an empty list
20  a new account of a deleted account's name does not get that account's old tokens
"""

import base64
import json
import os
import hashlib
import socket
import sys
import threading

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
        {"username": "boss", "password": "bosspass", "role": "admin"},
        {"username": "many", "password": "manypass", "role": "user"},
        {"username": "rate", "password": "ratepass", "role": "user"},
        {"username": "gone", "password": "gonepass", "role": "user"},
    ] + [
        {"username": "op%d" % i, "password": "op%dpass" % i, "role": "user"} for i in range(8)
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
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS}) as hub:
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

        print("[-] 4  listing: an operator sees its own tokens only")
        st, r = call("GET", "/api/auth/tokens", headers=bearer(app_tok))
        owners = {t.get("owner") for t in (r or {}).get("tokens", [])} if isinstance(r, dict) else None
        check(st == 200 and owners == {"guest"}, "4  operator sees only its own account's tokens", "%s %s" % (st, owners))

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

        print("[-] 8  the list shows owners")
        st, r = call("GET", "/api/auth/tokens", headers=A)
        owners = {t.get("id"): t.get("owner") for t in (r or {}).get("tokens", [])}
        check(st == 200 and owners.get(adm_id) == "admin", "8  the admin's token lists owner admin", str(owners))

        print("[-] 9  token made with a token")
        st, rb = make_token(basic("boss", "bosspass"), name="Android app (phone)")
        boss_app = rb.get("token")
        st, rh = make_token(bearer(boss_app), name="Home Assistant", role="admin")
        check(st == 201, "9  an admin's app token creates a token", "%s %s" % (st, rh))
        st, r = call("GET", "/api/auth/tokens", headers=A)
        owners = {t.get("id"): t.get("owner") for t in (r or {}).get("tokens", [])}
        check(owners.get(rh.get("id")) == "boss", "9  the new token belongs to boss", str(owners.get(rh.get("id"))))
        st, _ = call("POST", "/api/users/delete", {"username": "boss"}, A)
        st1, _ = call("GET", "/api/users", headers=bearer(boss_app))
        st2, _ = call("GET", "/api/users", headers=bearer(rh.get("token")))
        check(st1 == 401 and st2 == 401, "9  both end with the account", "%s %s" % (st1, st2))

        print("[-] 10  credential, not name")
        tokf = os.path.join(hub.data_dir, "config", "tokens.json")
        with open(tokf) as f:
            data = json.load(f)
        raw = "pat_" + "ab" * 16
        data["tokens"].append({"id": "tok_noname", "token_hash": hashlib.sha256(raw.encode()).hexdigest(),
                               "role": "user", "created_at": 1, "last_used_at": 0})
        with open(tokf, "w") as f:
            json.dump(data, f)
        st, r = make_token(bearer(raw), name="Planted")
        check(st == 403, "10 a nameless operator token cannot create a token", "%s %s" % (st, r))
        call("DELETE", "/api/auth/tokens?id=tok_noname", headers=A)

        print("[-] 11  limits")
        many = basic("many", "manypass")
        made = []
        for i in range(12):
            st, r = make_token(many, name="Android app %d" % i)
            made.append((st, r))
        codes = [st for st, _ in made]
        check(codes[:10] == [201] * 10, "11 ten tokens for one operator account", str(codes))
        check(codes[10] == 409 and "10" in str(made[10][1].get("error", "")), "11 the eleventh is refused (409)",
              "%s %s" % (codes[10], made[10][1]))
        st, _ = call("GET", "/api/status", headers=bearer(adm_pat))
        check(st == 200, "11 the admin's token still works", str(st))
        for _, r in made[:10]:
            call("DELETE", "/api/auth/tokens?id=%s" % r.get("id"), headers=many)
        st, r = call("GET", "/api/auth/tokens", headers=A)
        have = len((r or {}).get("tokens", []))
        codes = [make_token(A, name="Script %d" % i)[0] for i in range(70 - have)]
        check(codes.count(201) == 64 - have and codes[-1] == 409, "11 at most 64 tokens on the Hub",
              "had %d, %s" % (have, codes[-8:]))
        size = os.path.getsize(tokf)
        check(size < 40000, "11 a full list stays far below the read limit", "%d bytes" % size)
        st, _ = call("GET", "/api/status", headers=bearer(adm_pat))
        check(st == 200, "11 the admin's token still works with a full list", str(st))
        st, r = call("GET", "/api/auth/tokens", headers=A)
        for t in (r or {}).get("tokens", []):
            if t.get("name", "").startswith("Script "):
                call("DELETE", "/api/auth/tokens?id=%s" % t["id"], headers=A)

        print("[-] 12  unreadable token file")
        with open(tokf, "rb") as f:
            good = f.read()
        for label, content in (("cut", good[: len(good) // 2]), ("too big", b" " * 70000 + good)):
            with open(tokf, "wb") as f:
                f.write(content)
            st, r = make_token(A, name="After damage")
            st2, _ = call("DELETE", "/api/auth/tokens?id=%s" % adm_id, headers=A)
            st3, _ = call("GET", "/api/auth/tokens", headers=A)
            with open(tokf, "rb") as f:
                after = f.read()
            check(st == 500 and st2 == 500 and st3 == 500 and after == content,
                  "12 %s file: create, delete and list answer 500, file left alone" % label,
                  "%s %s %s" % (st, st2, st3))
        with open(tokf, "wb") as f:
            f.write(good)
        st, _ = call("GET", "/api/status", headers=bearer(adm_pat))
        check(st == 200, "12 the restored file works again", str(st))

        print("[-] 13  parallel creates and deletes")
        lost = resurrected = 0
        for rnd in range(15):
            G = basic("op%d" % (rnd % 8), "op%dpass" % (rnd % 8))  # within the per-account rate
            st, ha = make_token(A, name="HA %d" % rnd)
            got = []
            done = {}

            def mk():
                st, r = make_token(G, name="Android app")
                if st == 201:
                    got.append(r)

            def rm():
                done["st"] = call("DELETE", "/api/auth/tokens?id=%s" % ha.get("id"), headers=A)[0]

            ths = [threading.Thread(target=mk) for _ in range(4)]
            ths.insert(2, threading.Thread(target=rm))
            for t in ths:
                t.start()
            for t in ths:
                t.join()
            if done.get("st") == 200 and call("GET", "/api/status", headers=bearer(ha.get("token")))[0] == 200:
                resurrected += 1
            for r in got:
                if call("GET", "/api/status", headers=bearer(r["token"]))[0] != 200:
                    lost += 1
                call("DELETE", "/api/auth/tokens?id=%s" % r["id"], headers=G)
        check(lost == 0 and resurrected == 0, "13 no lost token, no deleted token back",
              "lost %d, back %d" % (lost, resurrected))

        print("[-] 14  password change")
        st, r = make_token(basic("other", "otherpass"), name="Android app")
        o_tok = r.get("token")
        o_sess = login("other", "otherpass")
        call("POST", "/api/users", {"username": "other", "password": "otherpass2"}, A)
        st1, _ = call("GET", "/api/status", headers=bearer(o_sess))
        st2, _ = call("GET", "/api/status", headers=bearer(o_tok))
        check(st1 == 401 and st2 == 200, "14 sessions end, the token keeps working", "%s %s" % (st1, st2))


        print("[-] 16  rate limit")
        R = basic("rate", "ratepass")
        codes = []
        for i in range(15):
            st, r = make_token(R, name="r%d" % i)
            codes.append(st)
            st2, _ = call("DELETE", "/api/auth/tokens?id=%s" % r.get("id"), headers=R)
            codes.append(st2)
        st, r = make_token(R, name="one too many")
        check(codes == [201, 200] * 15 and st == 429 and "try again" in str(r.get("error", "")),
              "16 the 31st token change in a minute is refused (429)", "%s %s" % (codes[-4:], st))

        print("[-] 17  operator's own tokens")
        O7 = basic("op7", "op7pass")
        st, a1 = make_token(O7, name="phone 1")
        st, a2 = make_token(O7, name="phone 2")
        st, l1 = call("GET", "/api/auth/tokens", headers=O7)
        ids = sorted(t["id"] for t in (l1 or {}).get("tokens", []))
        check(st == 200 and ids == sorted([a1["id"], a2["id"]]), "17 the list shows only the account's tokens", "%s %s" % (st, ids))
        st, l2 = call("GET", "/api/auth/tokens", headers=bearer(a1["token"]))
        ids2 = sorted(t["id"] for t in (l2 or {}).get("tokens", []))
        check(st == 200 and ids2 == ids, "17 the same with the account's token", "%s %s" % (st, ids2))
        st, _ = call("DELETE", "/api/auth/tokens?id=%s" % a2["id"], headers=bearer(a1["token"]))
        check(st == 200, "17 a token deletes another token of its account", str(st))
        call("DELETE", "/api/auth/tokens?id=%s" % a1["id"], headers=O7)

        print("[-] 18  reset of an unreadable file")
        with open(tokf, "rb") as f:
            good = f.read()
        st, r = call("POST", "/api/auth/tokens-reset", {"confirm": True}, A)
        check(st == 409, "18 a readable file is not reset", str(st))
        st, _ = call("POST", "/api/auth/tokens-reset", {"confirm": True}, bearer(adm_pat))
        check(st == 403, "18 not with a token", str(st))
        bad = good[: len(good) // 2]
        with open(tokf, "wb") as f:
            f.write(bad)
        st, r = make_token(A, name="x")
        check(st == 500 and "tokens-reset" in str((r or {}).get("error", "")), "18 the 500 says how to recover", str(r))
        st, _ = call("POST", "/api/auth/tokens-reset", {"confirm": True}, basic("guest", "guestpass"))
        check(st == 403, "18 an operator cannot reset", str(st))
        st, _ = call("POST", "/api/auth/tokens-reset", {}, A)
        check(st == 400, "18 not without confirm", str(st))
        with open(tokf, "rb") as f:
            check(f.read() == bad, "18 the refused calls left the file alone")
        st, r = call("POST", "/api/auth/tokens-reset", {"confirm": True}, A)
        with open(tokf + ".bad", "rb") as f:
            moved = f.read()
        with open(tokf) as f:
            fresh = json.load(f)
        check(st == 200 and moved == bad and fresh.get("tokens") == [], "18 moved to tokens.json.bad, empty list",
              "%s %s" % (st, r))
        st, r = make_token(A, name="after reset")
        check(st == 201, "18 tokens can be created again", str(st))

        print("[-] 20  a new account of the same name")
        with open(tokf, "w") as f:
            json.dump(fresh, f)
        st, rg = make_token(basic("gone", "gonepass"), name="old phone")
        with open(tokf) as f:
            with_gone = f.read()
        with open(tokf, "wb") as f:
            f.write(b"{cut")
        st, r = call("POST", "/api/users/delete", {"username": "gone"}, A)
        check(st == 200 and (r or {}).get("warning"), "20 delete with a broken file warns", str(r))
        st, r = call("POST", "/api/users", {"username": "gone", "password": "newpass1", "role": "admin"}, A)
        check(st == 500, "20 the name cannot be reused while the old tokens cannot be removed", str(st))
        with open(tokf, "w") as f:
            f.write(with_gone)
        st, r = call("POST", "/api/users", {"username": "gone", "password": "newpass1", "role": "admin"}, A)
        st2, _ = call("GET", "/api/users", headers=bearer(rg.get("token")))
        check(st == 200 and st2 == 401, "20 the old token does not come back with the new account", "%s %s" % (st, st2))

    print("[-] 15  switch on with an unreadable token file")
    off = {"auth_enabled": False, "users": [{"username": "admin", "password": "secret123", "role": "admin"}]}
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": off}) as hub:
        tokf = os.path.join(hub.data_dir, "config", "tokens.json")
        cut = b'{"tokens": [{"id": "tok_1", "name": "Old'
        with open(tokf, "wb") as f:
            f.write(cut)
        st, r = call("POST", "/api/users", {"username": "admin", "password": "secret123", "role": "admin",
                                            "auth_enabled": True})
        unc = (r or {}).get("to_check", {}) if isinstance(r, dict) else {}
        with open(tokf, "rb") as f:
            after = f.read()
        check(st == 200 and unc.get("tokens_unreadable") is True and after == cut,
              "15 the reply says the tokens are not listed, file left alone", "%s %s" % (st, r))

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] all checks passed")


if __name__ == "__main__":
    main()
