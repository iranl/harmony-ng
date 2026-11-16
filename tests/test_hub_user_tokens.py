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

import base64
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "hub_user.sh"
# hub_daemon_test for the tokens it writes itself (test_app_token.py uses 9300)
DAEMON_PORT = 9302


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
        self.write_tokens([token("tok_al", "alex"), token("tok_g", "guest")], "daemon")
        before = self.tokens_json.read_bytes()
        self.run_cmd("set-admin", "alex", "alexpass2")
        self.assertEqual(self.tokens_json.read_bytes(), before, "set-admin of an existing account changed the tokens")

    def test_not_a_token_list_is_left_alone(self):
        self.tokens_json.write_text('{"tokens": [ {"id": "tok_a", "owner": "guest"')
        before = self.tokens_json.read_bytes()
        r = self.run_cmd("delete", "guest")
        self.assertIn("does not look like a token list", r.stdout)
        self.assertEqual(self.tokens_json.read_bytes(), before)

    def test_no_token_file(self):
        self.run_cmd("delete", "guest")
        self.assertFalse(self.tokens_json.exists())

    def test_owner_text_in_a_name(self):
        """Token names that carry the text of an owner key, made by hub_daemon itself.

        hub_daemon writes each token as id, name, token_hash, role, owner (cJSON), so the name comes
        before the owner key. cJSON writes a quote in a name as \\" and a backslash as \\\\, so the
        name's text never reads as a key: only the owner key of each token counts."""
        names = [
            '"owner": "guest"',
            'x", "owner": "guest',
            '"owner":"guest"}, {"owner": "guest"',
            'owner',
            'a"owner',
            'ends with a backslash \\',
            'back\\slash \\"owner\\": \\"guest\\"',
            '[{"owner": "guest", "id": "tok_x"}]',
            'tab\tnew\nline "owner"\t:\n"guest"',
        ]
        users = {"auth_enabled": True, "users": [
            {"username": "alex", "password": "alexpass", "role": "admin"},
            {"username": "guest", "password": "guestpass", "role": "user"},
            {"username": "carol", "password": "carolpass", "role": "user"},
        ]}
        sys.path.insert(0, str(REPO_DIR / "tests"))
        from hub_harness import HubHarness  # noqa: E402
        made: dict[str, list[str]] = {}
        with HubHarness(port=DAEMON_PORT, host="127.0.0.2", extra_config_files={"users.json": users}) as hub:
            for account, pw, owner_names in (("alex", "alexpass", names),
                                             ("guest", "guestpass", ['"owner": "alex"', 'x", "owner": "alex']),
                                             ("carol", "carolpass", ['"owner": "guest"', "plain"])):
                auth = {"Authorization": "Basic " + base64.b64encode(f"{account}:{pw}".encode()).decode()}
                for nm in owner_names:
                    st, body = hub.http_post("/api/auth/tokens", {"name": nm, "role": "user"}, headers=auth)
                    self.assertEqual(st, 201, f"{account} {nm!r}: {st} {body!r}")
                    made.setdefault(account, []).append(body["id"])
            text = (pathlib.Path(hub.data_dir) / "config" / "tokens.json").read_text()
        written = json.loads(text)["tokens"]
        self.assertEqual(sorted(t["name"] for t in written if t["owner"] == "alex"), sorted(names))
        for t in written:
            at = text.index(t["id"])
            self.assertLess(text.index('"name"', at), text.index('"owner"', at),
                            "the daemon no longer writes the name before the owner")
        self.tokens_json.write_text(text)
        self.users_json.write_text(json.dumps({"auth_enabled": True, "users": [
            {"username": u, "ha1": h * 64, "role": r} for u, h, r in
            (("alex", "a", "admin"), ("guest", "b", "user"), ("carol", "c", "user"))]}, indent="\t"))

        r = self.run_cmd("delete", "guest")
        self.assertIn("Removed 2 access token(s) of 'guest'", r.stdout)
        self.assertEqual(self.tokens(), [t for t in written if t["owner"] != "guest"])
        r = self.run_cmd("delete", "carol")
        self.assertIn("Removed 2 access token(s) of 'carol'", r.stdout)
        self.assertEqual(self.tokens(), [t for t in written if t["owner"] == "alex"])
        self.assertEqual(sorted(t["id"] for t in self.tokens()), sorted(made["alex"]))

    def test_only_a_top_level_owner_key_counts(self):
        # not written by the Hub today: a token with a value that has an "owner" key of its own
        # before the token's owner key. Only the key of the token itself counts.
        toks = [{"id": "tok_a", "name": "n", "via": {"owner": "guest"}, "role": "user", "owner": "alex"},
                {"id": "tok_b", "name": "n", "via": [{"owner": "alex"}], "owner": "guest"},
                {"id": "tok_c", "name": "n", "owner": 7, "x": "guest"}]
        for layout in ("daemon", "compact"):
            with self.subTest(layout=layout):
                if layout == "compact":
                    self.tearDown()
                    self.setUp()
                self.write_tokens(toks, layout)
                r = self.run_cmd("delete", "guest")
                self.assertIn("Removed 1 access token(s)", r.stdout)
                self.assertEqual(self.tokens(), [toks[0], toks[2]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
