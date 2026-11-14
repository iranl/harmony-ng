#!/usr/bin/env python3
"""SSH key handling of remote/setup_remote.py (no network, ssh-keygen replaced).

remote/setup_remote.py must use <key>.pub as the public key of <key> (ssh-keygen's name),
and must never run "ssh-keygen -f" on a private key that exists (ssh-keygen then asks,
unseen, whether to overwrite it).
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR))


def load(rel: str, name: str):
    spec = importlib.util.spec_from_file_location(name, REPO_DIR / rel)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class SetupRemoteKey(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.mod = load("remote/setup_remote.py", "setup_remote_keys_under_test")
        self.calls: list[list[str]] = []

    def tearDown(self):
        self.tmp.cleanup()

    def fake_run(self, cmd, *args, **kwargs):
        self.calls.append(list(cmd))
        if cmd[:2] == ["ssh-keygen", "-y"]:
            return subprocess.CompletedProcess(cmd, 0, "ssh-ed25519 AAAAderived harmony-owner\n", "")
        return subprocess.CompletedProcess(cmd, 0, "", "")

    def ensure(self, key: pathlib.Path) -> pathlib.Path:
        with mock.patch.object(self.mod.subprocess, "run", self.fake_run), contextlib.redirect_stdout(io.StringIO()):
            return self.mod.ensure_ssh_key(key)

    def test_key_with_dot_in_name(self):
        key = self.dir / "hub.key"
        key.write_text("private\n")
        (self.dir / "hub.key.pub").write_text("ssh-ed25519 AAAAexisting\n")
        self.assertEqual(self.ensure(key), self.dir / "hub.key.pub")
        self.assertEqual(self.calls, [], "ssh-keygen must not run when the key pair exists")

    def test_missing_public_key_is_derived_not_regenerated(self):
        key = self.dir / "harmony_owner_ed25519"
        key.write_text("private\n")
        pub = self.ensure(key)
        self.assertEqual(pub, self.dir / "harmony_owner_ed25519.pub")
        self.assertFalse([c for c in self.calls if c[:1] == ["ssh-keygen"] and "-y" not in c],
                         "ssh-keygen was asked to generate a key over an existing private key")
        self.assertEqual(pub.read_text().strip(), "ssh-ed25519 AAAAderived harmony-owner")

    def test_new_key_generated(self):
        key = self.dir / "new_key"
        self.assertEqual(self.ensure(key), self.dir / "new_key.pub")
        self.assertTrue(any(c[:1] == ["ssh-keygen"] and "-f" in c for c in self.calls))


if __name__ == "__main__":
    unittest.main(verbosity=2)
