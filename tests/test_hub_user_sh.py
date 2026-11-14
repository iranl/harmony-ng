#!/usr/bin/env python3
"""payload/scripts/hub_user.sh on the host.

The script's /data and /tmp paths are moved into a temporary directory and the
copy runs with the host's sh. Checks that turning auth off and on again keeps
the accounts, that enable-auth refuses without an account, and that accounts
written by hub_daemon (users.json only, conf "# auth disabled") survive.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "hub_user.sh"

HA1_ADMIN = "a" * 64
HA1_GUEST = "b" * 64

# Same layout as cJSON_Print() in save_user_accounts() (hub_daemon)
DAEMON_USERS_JSON = (
    '{\n\t"auth_enabled":\tfalse,\n\t"realm":\t"Harmony Hub",\n\t"users":\t[{\n'
    f'\t\t\t"username":\t"admin",\n\t\t\t"ha1":\t"{HA1_ADMIN}",\n\t\t\t"role":\t"admin"\n'
    "\t\t}, {\n"
    f'\t\t\t"username":\t"guest",\n\t\t\t"ha1":\t"{HA1_GUEST}",\n\t\t\t"role":\t"user"\n'
    "\t\t}]\n}"
)


class HubUserScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "tmp").mkdir()
        text = SCRIPT.read_text()
        # /tmp first: the temporary root itself may be under /tmp
        text = text.replace('"/tmp/', f'"{self.root}/tmp/')
        text = text.replace("/data/harmony", str(self.root / "data" / "harmony"))
        self.script = self.root / "hub_user.sh"
        self.script.write_text(text)
        self.users_json = self.root / "data" / "harmony" / "config" / "users.json"
        self.auth_conf = self.root / "data" / "harmony" / "webui_auth.conf"

    def tearDown(self):
        self.tmp.cleanup()

    def run_cmd(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["sh", str(self.script), *args], capture_output=True, text=True, timeout=30)

    def users(self) -> dict:
        return json.loads(self.users_json.read_text())

    def names(self) -> list[str]:
        return [u["username"] for u in self.users()["users"]]

    def test_disable_then_enable_keeps_accounts(self):
        self.assertEqual(self.run_cmd("set-admin", "admin", "secret").returncode, 0)
        self.assertEqual(self.run_cmd("add-user", "guest", "pw").returncode, 0)
        ha1_before = {u["username"]: u["ha1"] for u in self.users()["users"]}

        r = self.run_cmd("disable-auth")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertFalse(self.users()["auth_enabled"])
        self.assertEqual(self.names(), ["admin", "guest"])

        r = self.run_cmd("list")
        self.assertIn("Username: admin", r.stdout)
        self.assertIn("Authentication is DISABLED", r.stdout)

        r = self.run_cmd("enable-auth")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertTrue(self.users()["auth_enabled"])
        self.assertEqual({u["username"]: u["ha1"] for u in self.users()["users"]}, ha1_before)
        self.assertIn("admin:" + ha1_before["admin"] + ":admin", self.auth_conf.read_text())

    def test_disable_twice_keeps_accounts(self):
        self.run_cmd("set-admin", "admin", "secret")
        self.run_cmd("disable-auth")
        self.run_cmd("disable-auth")
        self.assertEqual(self.names(), ["admin"])

    def test_enable_without_accounts_refused(self):
        r = self.run_cmd("enable-auth")
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn("ENABLED", r.stdout)
        # Comment-only conf, as left by hub_daemon with auth off and no accounts
        self.auth_conf.parent.mkdir(parents=True, exist_ok=True)
        self.auth_conf.write_text("# auth disabled\n")
        r = self.run_cmd("enable-auth")
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn("ENABLED", r.stdout)
        if self.users_json.exists():
            self.assertFalse(self.users()["auth_enabled"] and not self.users()["users"])

    def test_accounts_written_by_daemon(self):
        self.users_json.parent.mkdir(parents=True, exist_ok=True)
        self.users_json.write_text(DAEMON_USERS_JSON)
        self.auth_conf.write_text("# auth disabled\n")

        r = self.run_cmd("enable-auth")
        self.assertEqual(r.returncode, 0, r.stdout)
        u = self.users()
        self.assertTrue(u["auth_enabled"])
        self.assertEqual(
            [(x["username"], x["ha1"], x["role"]) for x in u["users"]],
            [("admin", HA1_ADMIN, "admin"), ("guest", HA1_GUEST, "user")],
        )

    def test_add_user_while_disabled_keeps_others(self):
        self.users_json.parent.mkdir(parents=True, exist_ok=True)
        self.users_json.write_text(DAEMON_USERS_JSON)
        self.auth_conf.write_text("# auth disabled\n")
        self.assertEqual(self.run_cmd("add-user", "kid", "pw").returncode, 0)
        self.assertEqual(self.names(), ["admin", "guest", "kid"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
