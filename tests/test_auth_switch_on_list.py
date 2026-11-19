#!/usr/bin/env python3
"""
When authentication goes from off to on, the reply lists the accounts and access tokens to check
(as agreed in #220): while it was off every client was admin, so anybody on the network could have
made or changed one.
- every path that turns it on answers "to_check": {"users": [...], "tokens": [...]}: POST /api/users
  with auth_enabled (the account it names is not listed), POST /api/system-action "auth" (the admin
  it sets is not listed), an import of a users file with authentication on (every account listed)
- every account but the named one and every token (id, name, owner, role; no hash), at every
  switch from off to on, again after an off period
- no list for a request that does not turn authentication on
- nothing is blocked: listed accounts and tokens keep working; no route marks or keeps anything
  (POST /api/auth/confirm does not exist), a "confirmed" field in an older file is ignored
Clients bind to 127.0.0.2 (not trusted as loopback). Port 9201, private mock roots.
"""

import base64
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9201
HOST = "127.0.0.2"
failures = []

TOKENS = {"tokens": [
    {"id": "tok_ghost001", "name": "ghost script", "token_hash": "0" * 64, "role": "admin", "owner": "ghost",
     "created_at": 1700000000, "last_used_at": 0},
    {"id": "tok_ha000001", "name": "Home Assistant", "token_hash": "1" * 64, "role": "user",
     "created_at": 1700000001, "last_used_at": 0, "confirmed": True},
]}


def check(cond, name, detail=""):
    print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
    if not cond:
        failures.append(name)


def basic(user, pw):
    return {"Authorization": "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode()).decode()}


def ha1(user, pw):
    return hashlib.sha256(("%s:Harmony Hub:%s" % (user, pw)).encode()).hexdigest()


def names(lst, key):
    return sorted(x.get(key) for x in (lst or []))


def toks_owner_missing(tc):
    return sorted(tk.get("id") for tk in tc.get("tokens", []) if tk.get("owner_missing") is True)


def run_users_route():
    print("[-] POST /api/users turns authentication on")
    # ghost exists before its token: creating an account removes tokens left by a deleted account
    # of the same name, so the token's owner must already be there
    ghost = {"auth_enabled": False, "users": [{"username": "ghost", "password": "pw-ghost", "role": "admin"}]}
    with HubHarness(port=PORT, host=HOST, extra_config_files={"tokens.json": TOKENS, "users.json": ghost}) as hub:
        st, r = hub.http_post("/api/users", {"username": "ghost", "password": "pw-ghost", "role": "admin"})
        check(st == 200 and "to_check" not in r, "account made while off, no list", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users", {"username": "spy", "password": "pw-spy", "role": "user"})
        st, r = hub.http_post("/api/users", {"username": "owner", "password": "pw-owner", "role": "admin",
                                             "auth_enabled": True})
        tc = (r or {}).get("to_check") or {}
        check(st == 200 and r.get("auth_enabled") is True and names(tc.get("users"), "username") == ["ghost", "spy"],
              "every account but the one named is listed", "%s %s" % (st, r))
        toks = {tk.get("id"): tk for tk in tc.get("tokens", [])}
        check(sorted(toks) == ["tok_ghost001", "tok_ha000001"] and toks["tok_ghost001"].get("owner") == "ghost"
              and toks["tok_ha000001"].get("name") == "Home Assistant"
              and all("token_hash" not in tk for tk in toks.values()), "every token, with owner and name, no hash", str(tc))

        owner = basic("owner", "pw-owner")
        st, _ = hub.http_get("/api/users")
        check(st == 401, "authentication is on", str(st))
        st, r = hub.http_get("/api/users", headers=basic("ghost", "pw-ghost"))
        check(st == 200, "nothing blocked: a listed admin still works", str(st))
        st, r = hub.http_get("/api/activity", headers=basic("spy", "pw-spy"))
        check(st == 200, "nothing blocked: a listed user still works", str(st))
        check(all("confirmed" not in u for u in (r if isinstance(r, dict) else {}).get("users", [])), "no marks")
        st, r = hub.http_post("/api/auth/confirm", {"all": True}, headers=owner)
        check(st == 404, "no route that keeps or marks anything", "%s %s" % (st, r))

        st, r = hub.http_post("/api/users", {"username": "later", "password": "pw-later", "role": "user"}, headers=owner)
        check(st == 200 and "to_check" not in r, "no list while authentication stays on", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users/delete", {"username": "ghost"}, headers=owner)
        check(st == 200, "delete with the usual route", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users", {"username": "owner", "auth_enabled": False}, headers=owner)
        check(st == 200 and "to_check" not in r, "turning it off: no list", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users", {"username": "owner", "auth_enabled": True})
        tc = (r or {}).get("to_check") or {}
        check(names(tc.get("users"), "username") == ["later", "spy"] and names(tc.get("tokens"), "id") == ["tok_ha000001"],
              "on again: every other account and token listed again", str(r))


def run_system_action():
    print("[-] POST /api/system-action auth (legacy web UI)")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"tokens.json": TOKENS}) as hub:
        st, r = hub.http_post("/api/system-action", {"action": "auth", "authUsername": "boss", "authPassword": "pw-boss",
                                                     "authEnabled": 0})
        check(st == 200 and "to_check" not in r, "admin set while staying off: no list", "%s %s" % (st, r))
        st, r = hub.http_post("/api/users", {"username": "kid", "password": "pw-kid", "role": "user"})
        st, r = hub.http_post("/api/system-action", {"action": "auth", "authEnabled": 1})
        tc = (r or {}).get("to_check") or {}
        check(st == 200 and names(tc.get("users"), "username") == ["kid"] and len(tc.get("tokens", [])) == 2,
              "the admin of the switch not listed, the others and every token are", "%s %s" % (st, r))


def run_import():
    print("[-] an import that turns authentication on")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"tokens.json": TOKENS}) as hub:
        payload = {"auth_enabled": True, "users": [
            {"username": "a1", "ha1": ha1("a1", "pw"), "role": "admin", "confirmed": True},
            {"username": "u1", "ha1": ha1("u1", "pw"), "role": "user"}]}
        st, r = hub.http_post("/import", {"target": "users", "payload": payload})
        tc = (r or {}).get("to_check") or {}
        check(st == 200 and r.get("auth_enabled") is True and names(tc.get("users"), "username") == ["a1", "u1"]
              and len(tc.get("tokens", [])) == 2, "every account and token listed", "%s %s" % (st, r))
        st, r = hub.http_get("/api/users", headers=basic("a1", "pw"))
        check(st == 200 and all("confirmed" not in u for u in r.get("users", [])),
              "an old \"confirmed\" field is ignored", "%s %s" % (st, r))
        st, r = hub.http_post("/import", {"target": "users", "payload": payload}, headers=basic("a1", "pw"))
        check(st == 200 and "to_check" not in r, "import with authentication already on: no list", "%s %s" % (st, r))
        check(toks_owner_missing(tc) == ["tok_ghost001"], "a token whose account is gone is marked owner_missing", str(tc))

    print("[-] the new web UI's restore: target all (the whole backup) turns authentication on")
    with HubHarness(port=PORT, host=HOST, extra_config_files={"tokens.json": TOKENS}) as hub:
        bundle = {"users": {"auth_enabled": True, "users": [{"username": "b1", "ha1": ha1("b1", "pw"), "role": "admin"}]}}
        for target in ("all", "bundle"):
            st, r = hub.http_post("/import", {"target": target, "payload": bundle},
                                  headers=basic("b1", "pw") if target == "bundle" else None)
            tc = (r or {}).get("to_check") or {}
            if target == "all":
                check(st == 200 and r.get("auth_enabled") is True and names(tc.get("users"), "username") == ["b1"]
                      and len(tc.get("tokens", [])) == 2, "target all is accepted and answers the list", "%s %s" % (st, r))
            else:
                check(st == 200 and "to_check" not in r, "bundle again with authentication on: no list", "%s %s" % (st, r))


def main():
    run_users_route()
    run_system_action()
    run_import()
    if failures:
        print("FAILED: %d check(s)" % len(failures))
        sys.exit(1)
    print("All auth switch checks passed")


if __name__ == "__main__":
    main()
