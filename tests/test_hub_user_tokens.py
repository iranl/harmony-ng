#!/usr/bin/env python3
"""payload/scripts/hub_user.sh and the access tokens of an account.

delete removes the account's tokens from tokens.json; add-user and set-admin with a name no account
has remove the tokens a deleted account of that name left (they would work again with the new
account); an update of an existing account keeps its tokens. The file is rewritten through a
temporary file only when a token was removed, stays valid JSON, keeps the other tokens with all
their fields, and a file that is not a token list is left alone.

Same host sandbox as test_hub_user_sh.py (/data and /tmp moved into a temporary directory).
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "hub_user.sh"


def token(tid: str, owner: str | None, name: str = "Android app (Pixel 7, 2026-11-15)") -> dict:
    t = {"id": tid, "name": name, "token_hash": tid.encode().hex().ljust(64, "0")[:64], "role": "user",
         "created_at": 1700000000, "last_used_at": 0}
    if owner:
        t["owner"] = owner
    return t


class HubUserTokens(unittest.TestCase):
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
        self.users_json.write_text(json.dumps({"auth_enabled": True, "users": [
            {"username": "alex", "ha1": "a" * 64, "role": "admin"},
            {"username": "guest", "ha1": "b" * 64, "role": "user"},
        ]}, indent="\t"))

    def tearDown(self):
        self.tmp.cleanup()

    def run_cmd(self, *args: str) -> subprocess.CompletedProcess[str]:
        r = subprocess.run(["sh", str(self.script), *args], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def tokens(self) -> list[dict]:
        return json.loads(self.tokens_json.read_text())["tokens"]

    def write_tokens(self, toks: list[dict], layout: str) -> None:
        doc = {"tokens": toks}
        text = json.dumps(doc, indent="\t") if layout == "daemon" else json.dumps(doc, separators=(",", ":"))
        self.tokens_json.write_text(text)

    def test_delete_removes_the_accounts_tokens(self):
        for layout in ("daemon", "compact"):
            with self.subTest(layout=layout):
                if layout == "compact":
                    self.tearDown()
                    self.setUp()
                odd = token("tok_c", "alex", name='name with "quotes", {braces} and ] bracket')
                self.write_tokens([token("tok_a", "guest"), odd, token("tok_b", "guest"), token("tok_d", None)], layout)
                r = self.run_cmd("delete", "guest")
                self.assertIn("Removed 2 access token(s)", r.stdout)
                self.assertEqual(self.tokens(), [odd, token("tok_d", None)])
                self.assertEqual(sorted(p.name for p in self.cfg.iterdir()), ["tokens.json", "users.json"])

    def test_new_name_removes_leftovers_existing_keeps(self):
        self.write_tokens([token("tok_old", "carol"), token("tok_g", "guest")], "daemon")
        before = self.tokens_json.read_bytes()
        self.run_cmd("add-user", "guest", "newpass1")
        self.assertEqual(self.tokens_json.read_bytes(), before, "an update of an existing account changed the tokens")
        self.run_cmd("add-user", "carol", "carolpass")
        self.assertEqual([t["id"] for t in self.tokens()], ["tok_g"])
        self.write_tokens([token("tok_old2", "boss"), token("tok_g", "guest")], "daemon")
        self.run_cmd("set-admin", "boss", "bosspass")
        self.assertEqual([t["id"] for t in self.tokens()], ["tok_g"])

    def test_not_a_token_list_is_left_alone(self):
        self.tokens_json.write_text('{"tokens": [ {"id": "tok_a", "owner": "guest"')
        before = self.tokens_json.read_bytes()
        r = self.run_cmd("delete", "guest")
        self.assertIn("does not look like a token list", r.stdout)
        self.assertEqual(self.tokens_json.read_bytes(), before)

    def test_no_token_file(self):
        self.run_cmd("delete", "guest")
        self.assertFalse(self.tokens_json.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
