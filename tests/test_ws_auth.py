#!/usr/bin/env python3
"""
WebSocket authentication on the upgrade (spec 3.8), against hub_daemon_test with auth enabled.
The daemon treats 127.0.0.1 as a trusted loopback caller (always admin), so the tests connect
through 127.0.0.2, which is loopback for the kernel but not for http_auth.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, WsClient, WsHandshakeError  # noqa: E402

PORT = 8995
USERS = {
    "auth_enabled": True,
    "users": [
        {"username": "admin", "password": "secret123", "role": "admin"},
        {"username": "guest", "password": "guestpass", "role": "user"},
    ],
}
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def try_upgrade(host, port, path="/ws", headers=None):
    """Returns (status, client or None)"""
    try:
        c = WsClient(host, port, path, headers=headers, timeout=3.0)
        return 101, c
    except WsHandshakeError as e:
        return e.status, None


def main():
    print("[-] 3.8 WebSocket upgrade with auth enabled (client 127.0.0.2)")
    with HubHarness(port=PORT, host="127.0.0.2", extra_config_files={"users.json": USERS}) as hub:
        st, s = hub.http_get("/api/status")
        check(st == 401, "REST without credentials is 401 from 127.0.0.2 (%s)" % st)
        st, login = hub.http_post("/api/auth/token", {"username": "admin", "password": "secret123"})
        session = login.get("access_token") if isinstance(login, dict) else None
        check(st == 200 and session and session.startswith("hht_"), "login gives a session token", str(login))
        bearer = {"Authorization": "Bearer %s" % session}
        st, pat = hub.http_post("/api/auth/tokens", {"name": "Home Assistant", "role": "admin"}, headers=bearer)
        pat_token = pat.get("token") if isinstance(pat, dict) else None
        pat_id = pat.get("id") if isinstance(pat, dict) else None
        check(st in (200, 201) and pat_token and pat_token.startswith("pat_"), "admin PAT created", "%s %s" % (st, pat))
        st, upat = hub.http_post("/api/auth/tokens", {"name": "Viewer", "role": "user"}, headers=bearer)
        user_pat = upat.get("token") if isinstance(upat, dict) else None
        check(st in (200, 201) and user_pat, "user-role PAT created", "%s %s" % (st, upat))

        status, c = try_upgrade("127.0.0.2", PORT)
        check(status == 401 and c is None, "upgrade without a token: 401 (%s)" % status)
        status, c = try_upgrade("127.0.0.2", PORT, "/ws?token=pat_not_a_real_token")
        check(status == 401, "upgrade with an unknown token: 401 (%s)" % status)

        status, c = try_upgrade("127.0.0.2", PORT, "/ws?token=%s" % pat_token)
        check(status == 101 and c is not None, "PAT in the query string: 101 (%s)" % status)
        if c:
            c.send_json({"action": "ping", "msgId": 1})
            msg, _ = c.wait_for("pong", 3)
            check(msg is not None and msg.get("msgId") == 1, "client works after the upgrade", str(msg))
            c.close()

        status, c = try_upgrade("127.0.0.2", PORT, "/ws", headers={"Authorization": "Bearer %s" % pat_token})
        check(status == 101, "PAT in the Authorization header: 101 (%s)" % status)
        if c:
            c.close()

        status, c = try_upgrade("127.0.0.2", PORT, "/ws?token=%s" % session)
        check(status == 101, "session token in the query string: 101 (%s)" % status)
        if c:
            c.close()

        # user role: connects, ping and get_state allowed, command actions follow the REST table
        status, c = try_upgrade("127.0.0.2", PORT, "/ws?token=%s" % user_pat)
        check(status == 101, "user-role PAT: 101 (%s)" % status)
        if c:
            c.send_json({"action": "get_state", "msgId": 2})
            msg, _ = c.wait_for("activity_state", 3, lambda m: m.get("msgId") == 2)
            check(msg is not None, "user role may query the state", str(msg))
            c.send_json({"action": "start_activity", "id": "10", "msgId": 3})
            msg, _ = c.wait_for("ack", 5, lambda m: m.get("msgId") == 3)
            check(msg is not None and msg.get("ok") is True, "user role may start an activity (permitted for REST too)", str(msg))
            c.close()

        # revoked PAT
        st, r = hub.http_post("/api/auth/tokens/delete", {"id": pat_id}, headers=bearer)
        check(st == 200, "PAT revoked (%s)" % st, str(r))
        status, c = try_upgrade("127.0.0.2", PORT, "/ws?token=%s" % pat_token)
        check(status == 401, "revoked PAT: 401 (%s)" % status)

        # loopback proper stays trusted (hub_rf, hub_btstack)
        status, c = try_upgrade("127.0.0.1", PORT)
        check(status == 101, "127.0.0.1 without a token: 101 (%s)" % status)
        if c:
            c.close()

    print("[-] 3.8 auth disabled: unchanged")
    with HubHarness(port=PORT, host="127.0.0.2") as hub:
        status, c = try_upgrade("127.0.0.2", PORT)
        check(status == 101, "upgrade without a token when auth is off: 101 (%s)" % status)
        if c:
            c.close()
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all WebSocket auth checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
