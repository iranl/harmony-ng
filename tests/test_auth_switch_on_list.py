#!/usr/bin/env python3
"""
Accounts and personal access tokens made while authentication was off are listed when it is
turned on (as agreed in #220). With authentication off every client is admin, so an account or a
token made then could have been planted by anybody on the network.
- an account made while authentication is off is not confirmed (GET /api/users "confirmed": false);
  one made while it is on, and the owner's account named in the request that turns it on, are
- POST /api/users with auth_enabled true (off -> on) answers "unconfirmed": {"users", "tokens"}:
  the other unconfirmed accounts and the tokens not marked "confirmed" (older files); a token made
  with authentication on is confirmed and not listed
- POST /api/system-action "auth" (the legacy web UI's switch) answers the same list
- POST /api/auth/confirm keeps the named accounts and tokens (or all), deleting uses the usual
  routes; a later off -> on switch lists nothing kept
- a request that does not turn authentication on carries no list
Clients bind to 127.0.0.2 (not trusted as loopback). Port 9201, private mock roots.
"""

import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9201
HOST = "127.0.0.2"
failures = []

LEGACY_TOKENS = {"tokens": [
    {"id": "tok_legacy01", "name": "made by an old firmware", "token_hash": "0" * 64, "role": "admin",
     "created_at": 1700000000, "last_used_at": 0},
    {"id": "tok_kept0001", "name": "made with auth on", "token_hash": "1" * 64, "role": "user",
     "created_at": 1700000001, "last_used_at": 0, "confirmed": True},
]}


def check(cond, name, detail=""):
    print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
    if not cond:
        failures.append(name)


def basic(user, pw):
    return {"Authorization": "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode()).decode()}


def names(lst, key):
    return sorted(x.get(key) for x in (lst or []))


def users_by_name(hub, headers=None):
    st, r = hub.http_get("/api/users", headers=headers)
    return {u["username"]: u for u in (r.get("users", []) if isinstance(r, dict) else [])}


def run_users_route():
    print("[-] switch through POST /api/users")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"tokens.json": LEGACY_TOKENS}) as hub:
        st, r = hub.http_post("/api/users", {"username": "ghost", "password": "pw-ghost", "role": "admin"})
        check(st == 200, "account made while off (admin)", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users", {"username": "spy", "password": "pw-spy", "role": "user"})
        check(st == 200 and "unconfirmed" not in r, "account made while off (user), no list", "%s %s" % (st, r))
        us = users_by_name(hub)
        check(us.get("ghost", {}).get("confirmed") is False and us.get("spy", {}).get("confirmed") is False,
              "accounts made while off are not confirmed", str(us))

        st, r = hub.http_post("/api/users", {"username": "owner", "password": "pw-owner", "role": "admin",
                                             "auth_enabled": True})
        unc = r.get("unconfirmed") if isinstance(r, dict) else None
        check(st == 200 and isinstance(unc, dict), "turning auth on answers the list", "%s %s" % (st, r))
        check(unc is not None and names(unc.get("users"), "username") == ["ghost", "spy"],
              "the other accounts are listed, not the owner's", str(unc))
        check(unc is not None and names(unc.get("tokens"), "id") == ["tok_legacy01"]
              and unc["tokens"][0].get("role") == "admin" and "token_hash" not in unc["tokens"][0],
              "the token without the confirmed mark is listed (no hash)", str(unc))

        st, _ = hub.http_get("/api/users")
        check(st == 401, "authentication is on", str(st))
        owner = basic("owner", "pw-owner")
        us = users_by_name(hub, owner)
        check(us.get("owner", {}).get("confirmed") is True, "the owner's account is confirmed", str(us))

        st, r = hub.http_post("/api/auth/tokens", {"name": "integration"}, headers=owner)
        check(st == 201, "token made with auth on", "%s %s" % (st, r))
        st, r = hub.http_get("/api/auth/tokens", headers=owner)
        toks = {t["id"]: t for t in (r.get("tokens", []) if isinstance(r, dict) else [])}
        new = [t for t in toks.values() if t.get("name") == "integration"]
        check(len(new) == 1 and new[0].get("confirmed") is True and toks["tok_legacy01"].get("confirmed") is False,
              "GET /api/auth/tokens shows confirmed", str(toks))

        st, r = hub.http_post("/api/auth/confirm", {"users": ["spy"], "tokens": ["tok_legacy01"]}, headers=owner)
        check(st == 200 and r.get("users") == 1 and r.get("tokens") == 1, "keep one account and one token", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users/delete", {"username": "ghost"}, headers=owner)
        check(st == 200, "delete the other account with the usual route", "%s %s" % (st, r))
        st, r = hub.http_post("/api/auth/confirm", {"all": True}, headers=basic("spy", "pw-spy"))
        check(st in (401, 403), "confirm needs the admin role", str(st))
        st, r = hub.http_post("/api/auth/confirm", "", headers=owner, raw=b"not json")
        check(st == 400, "confirm needs a JSON object", str(st))

        st, r = hub.http_post("/api/users", {"username": "later", "password": "pw-later", "role": "user"}, headers=owner)
        check(st == 200 and "unconfirmed" not in r, "account made with auth on, no list", "%s %s" % (st, r))
        us = users_by_name(hub, owner)
        check(sorted(us) == ["later", "owner", "spy"] and all(u.get("confirmed") for u in us.values()),
              "kept, owner's and later accounts are confirmed", str(us))

        st, r = hub.http_post("/api/users", {"username": "owner", "auth_enabled": False}, headers=owner)
        check(st == 200 and "unconfirmed" not in r, "auth off again", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users", {"username": "owner", "auth_enabled": True})
        unc = r.get("unconfirmed") if isinstance(r, dict) else None
        check(st == 200 and unc == {"users": [], "tokens": []}, "on again: nothing left to ask about", "%s %s" % (st, r))


def run_system_action():
    print("[-] switch through POST /api/system-action auth (legacy web UI)")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"tokens.json": LEGACY_TOKENS}) as hub:
        st, r = hub.http_post("/api/system-action", {"action": "auth", "authUsername": "boss",
                                                     "authPassword": "pw-boss", "authEnabled": 0})
        check(st == 200 and "unconfirmed" not in r, "admin set while staying off: no list", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users", {"username": "spy", "password": "pw-spy", "role": "user"})
        check(st == 200, "user account made while off", "%s %s" % (st, r))
        st, r = hub.http_post("/api/system-action", {"action": "auth", "authUsername": "boss",
                                                     "authPassword": "pw-boss", "authEnabled": 1})
        unc = r.get("unconfirmed") if isinstance(r, dict) else None
        check(st == 200 and unc is not None and names(unc.get("users"), "username") == ["spy"]
              and names(unc.get("tokens"), "id") == ["tok_legacy01"], "list in the auth action's reply", "%s %s" % (st, r))
        us = users_by_name(hub, basic("boss", "pw-boss"))
        check(us.get("boss", {}).get("confirmed") is True and us.get("spy", {}).get("confirmed") is False,
              "the admin of the switch is confirmed, the other is not", str(us))
        st, r = hub.http_post("/api/auth/confirm", {"all": True}, headers=basic("boss", "pw-boss"))
        check(st == 200 and r.get("users") == 1 and r.get("tokens") == 1, "keep all", "%s %s" % (st, r))
        with open(os.path.join(hub.data_dir, "config", "tokens.json")) as f:
            saved = json.load(f)
        check(all(t.get("confirmed") is True for t in saved.get("tokens", [])), "tokens.json keeps the mark", str(saved))


def main():
    run_users_route()
    run_system_action()
    if failures:
        print("FAILED: %d check(s)" % len(failures))
        sys.exit(1)
    print("All auth switch checks passed")


if __name__ == "__main__":
    main()
