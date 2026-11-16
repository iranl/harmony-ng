#!/usr/bin/env python3
"""
Unused route aliases are gone, their canonical routes still work (hub_daemon_test, auth on,
admin session from 127.0.0.2).

 1  GET /api/wifi/scan answers 404; GET /api/wifi-scan answers 200 with a network list
 2  POST /api/auth/tokens/delete answers 404 and leaves the token in place;
    DELETE /api/auth/tokens?id=<id> removes it
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, _http  # noqa: E402

PORT = 9340
HOST = "127.0.0.2"
USERS = {
    "auth_enabled": True,
    "users": [{"username": "admin", "password": "secret123", "role": "admin"}],
}
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def token_ids(hub, headers):
    st, r = hub.http_get("/api/auth/tokens", headers=headers)
    items = r.get("tokens", r) if isinstance(r, dict) else r
    if not isinstance(items, list):
        return st, None
    return st, [t.get("id") for t in items if isinstance(t, dict)]


def main():
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS}) as hub:
        st, login = hub.http_post("/api/auth/token", {"username": "admin", "password": "secret123"})
        session = login.get("access_token") if isinstance(login, dict) else None
        check(st == 200 and session, "admin logs in", "%s %s" % (st, login))
        auth = {"Authorization": "Bearer %s" % session}

        print("[-] 1  Wi-Fi scan has one name")
        st, r = hub.http_get("/api/wifi/scan", headers=auth)
        check(st == 404, "GET /api/wifi/scan is 404", "%s %s" % (st, r))
        st, r = hub.http_get("/api/wifi-scan", headers=auth, timeout=10.0)
        check(st == 200 and isinstance(r, dict) and isinstance(r.get("networks"), list),
              "GET /api/wifi-scan still answers with a network list", "%s %s" % (st, r))

        print("[-] 2  token revocation has one name")
        st, pat = hub.http_post("/api/auth/tokens", {"name": "Client", "role": "user"}, headers=auth)
        pat_id = pat.get("id") if isinstance(pat, dict) else None
        check(st in (200, 201) and pat_id, "token created", "%s %s" % (st, pat))
        st, r = hub.http_post("/api/auth/tokens/delete", {"id": pat_id}, headers=auth)
        check(st == 404, "POST /api/auth/tokens/delete is 404", "%s %s" % (st, r))
        st, ids = token_ids(hub, auth)
        check(ids is not None and pat_id in ids, "token still listed after the old name", "%s %s" % (st, ids))
        st, r = _http(hub.host, hub.port, "DELETE", "/api/auth/tokens?id=%s" % pat_id, None, auth, 3.0)
        check(st == 200, "DELETE /api/auth/tokens removes it", "%s %s" % (st, r))
        st, ids = token_ids(hub, auth)
        check(ids is not None and pat_id not in ids, "token gone after DELETE", "%s %s" % (st, ids))

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] removed route aliases: all checks passed")


if __name__ == "__main__":
    main()
