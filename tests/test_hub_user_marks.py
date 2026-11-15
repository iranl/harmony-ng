#!/usr/bin/env python3
"""payload/scripts/hub_user.sh keeps hub_daemon's account marks ("confirmed").

Same host sandbox as test_hub_user_sh.py (/data and /tmp moved into a temporary directory). Checks
that a rewrite keeps every account's mark, that the account the command sets (set-admin, add-user)
is kept (root over SSH is the owner), that enable-auth from off marks accounts and tokens without a
mark false like hub_daemon does at that switch, that set-admin and enable-auth print the accounts
and tokens still waiting, and that the files stay valid JSON.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "hub_user.sh"


def daemon_users(auth_on: bool, users: list[dict]) -> str:
    """users.json as hub_daemon writes it (cJSON_Print layout)"""
    return json.dumps({"auth_enabled": auth_on, "realm": "Harmony Hub", "users": users}, indent="\t")


class HubUserMarks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "tmp").mkdir()
        text = SCRIPT.read_text()
        text = text.replace('"/tmp/', f'"{self.root}/tmp/')
        text = text.replace("/data/harmony", str(self.root / "data" / "harmony"))
        self.script = self.root / "hub_user.sh"
        self.script.write_text(text)
        self.cfg = self.root / "data" / "harmony" / "config"
        self.cfg.mkdir(parents=True)
        self.users_json = self.cfg / "users.json"
        self.tokens_json = self.cfg / "tokens.json"

    def tearDown(self):
        self.tmp.cleanup()

    def run_cmd(self, *args: str) -> subprocess.CompletedProcess[str]:
        r = subprocess.run(["sh", str(self.script), *args], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def marks(self) -> dict:
        return {u["username"]: u.get("confirmed") for u in json.loads(self.users_json.read_text())["users"]}

    def test_rewrite_keeps_marks(self):
        self.users_json.write_text(daemon_users(True, [
            {"username": "alex", "ha1": "a" * 64, "role": "admin", "confirmed": True},
            {"username": "kid", "ha1": "b" * 64, "role": "user", "confirmed": False},
            {"username": "old", "ha1": "c" * 64, "role": "user"},
        ]))
        self.run_cmd("add-user", "guest", "pw")
        self.assertEqual(self.marks(), {"alex": True, "kid": False, "old": None, "guest": True})
        self.run_cmd("delete", "guest")
        self.assertEqual(self.marks(), {"alex": True, "kid": False, "old": None})

    def test_enable_auth_marks_and_prints(self):
        self.users_json.write_text(daemon_users(False, [
            {"username": "alex", "ha1": "a" * 64, "role": "admin", "confirmed": True},
            {"username": "planted", "ha1": "b" * 64, "role": "admin"},
        ]))
        self.tokens_json.write_text(json.dumps({"tokens": [
            {"id": "tok_old", "name": "old", "token_hash": "0" * 64, "role": "admin", "created_at": 1},
            {"id": "tok_ok", "name": "ok", "token_hash": "1" * 64, "role": "user", "created_at": 2, "confirmed": True},
        ]}, indent="\t"))
        r = self.run_cmd("enable-auth")
        self.assertEqual(self.marks(), {"alex": True, "planted": False})
        toks = {t["id"]: t.get("confirmed") for t in json.loads(self.tokens_json.read_text())["tokens"]}
        self.assertEqual(toks, {"tok_old": False, "tok_ok": True})
        self.assertIn("planted", r.stdout)
        self.assertIn("token tok_old", r.stdout)
        self.assertNotIn("alex", r.stdout.split("not kept yet:")[1])

    def test_set_admin_keeps_its_account(self):
        self.users_json.write_text(daemon_users(False, [
            {"username": "boss", "ha1": "a" * 64, "role": "admin", "confirmed": False},
            {"username": "kid", "ha1": "b" * 64, "role": "user"},
        ]))
        r = self.run_cmd("set-admin", "boss", "new-pw")
        self.assertEqual(self.marks(), {"boss": True, "kid": False})
        self.assertIn("kid", r.stdout)


if __name__ == "__main__":
    unittest.main()
