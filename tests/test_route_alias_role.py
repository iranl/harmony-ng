#!/usr/bin/env python3
"""
Two small hub_daemon contracts, against hub_daemon_test with authentication on (clients bind
to 127.0.0.2: loopback for the kernel, not for http_auth).

 1  POST /api/mqtt-publish is the older name of POST /api/button-press (same handler): a
    `user` account gets the same answer on both names, not 403 on one of them
 2  the WebSocket snapshot's hub object carries each key once (it had hub_id twice, which strict
    JSON parsers refuse)
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9263
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


def duplicate_keys(pairs):
    seen, dups = set(), []
    for k, _ in pairs:
        if k in seen:
            dups.append(k)
        seen.add(k)
    return dict(pairs) if not dups else {"__duplicates__": dups}


def main():
    with HubHarness(port=PORT, host=HOST, extra_config_files={"users.json": USERS}) as hub:
        st, r = hub.http_post("/api/auth/token", {"username": "guest", "password": "guestpass"})
        tok = r.get("access_token") if st == 200 and isinstance(r, dict) else None
        check(tok is not None, "user account logs in", "%s %s" % (st, r))
        auth = {"Authorization": "Bearer %s" % tok}

        print("[-] 1  /api/mqtt-publish needs the same role as /api/button-press")
        body = {"deviceId": "1", "button": "VolumeUp", "source": "api"}
        st_a, r_a = hub.http_post("/api/button-press", body, headers=auth)
        st_b, r_b = hub.http_post("/api/mqtt-publish", body, headers=auth)
        check(st_a != 403, "user may post /api/button-press", "%s %s" % (st_a, r_a))
        check(st_b == st_a, "user gets the same answer on /api/mqtt-publish",
              "button-press %s, mqtt-publish %s %s" % (st_a, st_b, r_b))

        print("[-] 2  snapshot hub object has no duplicate key")
        ws = hub.ws(headers=auth)
        snap = None
        for _ in range(10):
            fr = ws.recv_frame(timeout=3)
            if fr is None:
                break
            op, data = fr
            if op == 1 and b'"snapshot"' in data:
                snap = data.decode("utf-8")
                break
        ws.close()
        check(snap is not None, "snapshot received")
        if snap:
            obj = json.loads(snap, object_pairs_hook=duplicate_keys)
            hub_obj = obj.get("hub", {})
            check("__duplicates__" not in hub_obj, "hub object keys are unique",
                  str(hub_obj.get("__duplicates__")))
            check(isinstance(hub_obj, dict) and hub_obj.get("hub_id"), "hub object has hub_id")

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] route alias role and snapshot keys: all checks passed")


if __name__ == "__main__":
    main()
