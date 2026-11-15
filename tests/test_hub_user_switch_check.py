#!/usr/bin/env python3
"""payload/scripts/hub_user.sh prints the accounts and access tokens to check when it turns
authentication on (as hub_daemon answers at that switch), and leaves the files alone otherwise.

Same host sandbox as test_hub_user_sh.py (/data and /tmp moved into a temporary directory).
Checks: enable-auth from off lists every account and every token (a token file in any layout);
set-admin from off does not list the account it sets; a command with authentication already on
prints no list; tokens.json is never changed; a "confirmed" field in an older users.json is ignored.
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


class HubUserSwitchCheck(unittest.TestCase):
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

    def test_enable_auth_lists_everything(self):
        self.users_json.write_text(daemon_users(False, [
            {"username": "alex", "ha1": "a" * 64, "role": "admin", "confirmed": True},
            {"username": "planted", "ha1": "b" * 64, "role": "admin"},
        ]))
        compact = '{"tokens":[{"id":"tok_a","name":"script","token_hash":"%s","role":"admin"},' \
                  '{"id":"tok_b","name":"HA","token_hash":"%s","role":"user"}]}' % ("0" * 64, "1" * 64)
        self.tokens_json.write_text(compact)
        r = self.run_cmd("enable-auth")
        self.assertIn("Check the accounts and access tokens", r.stdout)
        for item in ("account alex (admin)", "account planted (admin)", "token tok_a script (admin)", "token tok_b HA (user)"):
            self.assertIn(item, r.stdout)
        self.assertEqual(self.tokens_json.read_text(), compact, "tokens.json not touched")
        self.assertTrue(json.loads(self.users_json.read_text())["auth_enabled"])

    def test_set_admin_does_not_list_its_account(self):
        self.users_json.write_text(daemon_users(False, [
            {"username": "boss", "ha1": "a" * 64, "role": "admin"},
            {"username": "kid", "ha1": "b" * 64, "role": "user"},
        ]))
        r = self.run_cmd("set-admin", "boss", "new-pw")
        self.assertIn("account kid (user)", r.stdout)
        self.assertNotIn("account boss", r.stdout)

    def test_no_list_when_already_on(self):
        self.users_json.write_text(daemon_users(True, [{"username": "alex", "ha1": "a" * 64, "role": "admin"}]))
        r = self.run_cmd("add-user", "guest", "pw")
        self.assertNotIn("Check the accounts", r.stdout)

    def test_accounts_only_in_the_conf_count_as_on(self):
        """no users.json, accounts in webui_auth.conf: hub_daemon counts that as on, so no switch"""
        (self.root / "data" / "harmony" / "webui_auth.conf").write_text("alex:%s:admin\n" % ("a" * 64))
        r = self.run_cmd("add-user", "guest", "pw")
        self.assertNotIn("Check the accounts", r.stdout)


if __name__ == "__main__":
    unittest.main()
