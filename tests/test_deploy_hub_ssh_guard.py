#!/usr/bin/env python3
"""build/deploy_hub.py installs the Ed25519-only SSH server only when it cannot lock this PC out.

- ED25519_USER_KEY_CHECK (the shell text sent to the Hub) counts a real Ed25519 key line, also one
  with options in front, and not a comment, a missing file, or "ssh-ed25519" in another key's
  comment.
- ed25519_login_problem(): no key, a failed check, and a key of another PC (the Ed25519-only login
  of this PC fails) each give a reason; a key this PC logs in with gives none.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))

import deploy_hub  # noqa: E402

ED = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFakeFakeFakeFakeFakeFake owner\n"
RSA = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQfake owner\n"


class KeyCheckText(unittest.TestCase):
    def answer(self, keys: str | None) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "etc").mkdir()
            (root / "etc/passwd").write_text(f"root:x:0:0:root:{root}/home/root:/bin/sh\n")
            if keys is not None:
                (root / "home/root/.ssh").mkdir(parents=True)
                (root / "home/root/.ssh/authorized_keys").write_text(keys)
            text = deploy_hub.ED25519_USER_KEY_CHECK.replace("/etc/passwd", f"{root}/etc/passwd")
            return subprocess.run(["sh", "-c", text], capture_output=True, text=True).stdout.strip()

    def test_cases(self):
        self.assertEqual(self.answer(ED), "yes")
        self.assertEqual(self.answer(RSA + 'no-pty,command="x" ' + ED), "yes")
        self.assertEqual(self.answer(None), "no")
        self.assertEqual(self.answer(RSA), "no")
        self.assertEqual(self.answer(RSA + "# " + ED), "no")
        self.assertEqual(self.answer(RSA + "\t# " + ED), "no")
        self.assertEqual(self.answer("ssh-rsa AAAAB3Nfake replaces ssh-ed25519 key of old pc\n"), "no")


class LoginProblem(unittest.TestCase):
    def make(self, check_out: str, check_rc: int = 0):
        d = deploy_hub.HubDeployer(hub_host="hub.invalid", ssh_key="/nonexistent", no_backup=True)
        d.run_ssh = lambda cmd: subprocess.CompletedProcess(cmd, check_rc, check_out, "boom" if check_rc else "")
        return d

    def probe(self, d, rc: int):
        seen = []

        def fake_run(cmd, *a, **k):
            seen.append(cmd)
            return subprocess.CompletedProcess(cmd, rc, "", "")
        with mock.patch.object(deploy_hub.subprocess, "run", fake_run):
            return d.ed25519_login_problem(), seen

    def test_no_key(self):
        problem, seen = self.probe(self.make("no\n"), 0)
        self.assertIn("no Ed25519 key", problem)
        self.assertEqual(seen, [], "no login test without a key")

    def test_check_failed_is_not_no_key(self):
        problem, _ = self.probe(self.make("", 255), 0)
        self.assertIn("could not be checked", problem)

    def test_key_of_another_pc(self):
        problem, seen = self.probe(self.make("yes\n"), 255)
        self.assertIn("this PC cannot log in", problem)
        self.assertIn("PubkeyAcceptedKeyTypes=ssh-ed25519", seen[0])

    def test_this_pc_logs_in(self):
        problem, _ = self.probe(self.make("yes\n"), 0)
        self.assertIsNone(problem)


if __name__ == "__main__":
    unittest.main(verbosity=2)
