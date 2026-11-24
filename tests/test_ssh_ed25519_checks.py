#!/usr/bin/env python3
"""The Ed25519 key checks that decide whether the Hub gets the Ed25519-only SSH server.

- build/deploy_hub.py (ED25519_USER_KEY_CHECK, sent to the Hub) and payload/scripts/update.sh
  (has_ed25519_user_key(), on the Hub during an OTA) use the same two grep patterns and give the
  same answer for every line below. "yes" only for a line in the shape the release's dropbear
  reads for the client's Ed25519 key: the key type at the start of the line, or after one options
  field and one blank, then one space and the whole 68-character key blob (option names are not
  checked: a line with an option dropbear does not know is a yes it refuses). An RSA key carrying an Ed25519 key as
  its comment, an RSA line and an Ed25519 line joined on one line, a cut key and a key with blanks
  in front but no options (dropbear reads the type as an options field) are "no".
- With HARMONY_HUB_SH set to a command that runs the stock Hub shell (BusyBox 1.13.4, e.g. under
  qemu-mips with its applets first in PATH), the table also runs there.
- tools/root_hub_usb.py picks the newest Ed25519 harmony_owner key also when it does not authorize
  one (restore_stock.py --skip-usb-root), not a newer RSA key.
"""

from __future__ import annotations

import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

import deploy_hub  # noqa: E402
from tools import root_hub_usb  # noqa: E402

BLOB = "AAAAC3NzaC1lZDI1NTE5AAAAIAiiACbtw7zFBaJ9uICKf3GGoYrHCkZPnK1d7B0o4saR"
ED = f"ssh-ed25519 {BLOB} owner@pc"
RSA = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQDGuardProbeKeyForTheTestOnly owner@pc"

# (label, authorized_keys text, answer); "no" lines are lines the release's dropbear refuses
# for this key (checked with the server under qemu-mips), or never sees (no file)
CASES = (
    ("plain", f"{ED}\n", "yes"),
    ("no comment", f"ssh-ed25519 {BLOB}\n", "yes"),
    ("no newline at the end", ED, "yes"),
    ("CRLF line ends", f"{ED}\r\n", "yes"),
    ("options in front", f'no-pty,command="a b" {ED}\n', "yes"),
    ("blanks, then options", f"  no-pty {ED}\n", "yes"),
    ("tab after the options", f"no-pty\t{ED}\n", "yes"),
    ("RSA line, then Ed25519 line", f"{RSA}\n{ED}\n", "yes"),
    ("ssh-rsa in the Ed25519 key's comment", f"{ED} replaces ssh-rsa key\n", "yes"),
    ("no file", None, "no"),
    ("empty file", "", "no"),
    ("RSA only", f"{RSA}\n", "no"),
    ("commented out", f"{RSA}\n# {ED}\n", "no"),
    ("commented out after a blank and a tab", f"{RSA}\n \t# {ED}\n", "no"),
    ("the word in an RSA key's comment", "ssh-rsa AAAAB3Nfake replaces ssh-ed25519 key of old pc\n", "no"),
    ("RSA key with a whole Ed25519 key as its comment", f"{RSA.rsplit(' ', 1)[0]} {ED}\n", "no"),
    ("RSA line and Ed25519 line joined", f"{RSA} {ED}\n", "no"),
    ("RSA line and Ed25519 line joined, no blank", f"{RSA}{ED}\n", "no"),
    ("key cut after the prefix", "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI owner@pc\n", "no"),
    ("cut key, then a whole one", f"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI {ED}\n", "no"),
    ("blob one character too long", f"ssh-ed25519 {BLOB}A owner@pc\n", "no"),
    ("blanks in front, no options", f"  {ED}\n", "no"),
    ("tab between type and blob", f"ssh-ed25519\t{BLOB} owner@pc\n", "no"),
    ("two blanks after the options", f"no-pty  {ED}\n", "no"),
    ("another key type as first word", f"ssh-rsa {ED}\n", "no"),
    ("ECDSA type as first word", f"ecdsa-sha2-nistp256 {ED}\n", "no"),
    ("type twice", f"ssh-ed25519 {ED}\n", "no"),
    ("FIDO key with an Ed25519 key as comment",
     f"sk-ssh-ed25519@openssh.com AAAAGnNrLXNzaC1lZDI1NTE5QG9wZW5zc2guY29t {ED}\n", "no"),
)

# The three lines of the report (F7) that the check before this change took for a key
REPORTED = ("RSA key with a whole Ed25519 key as its comment", "RSA line and Ed25519 line joined",
            "key cut after the prefix")


def update_sh_function() -> str:
    text = (REPO_DIR / "payload" / "scripts" / "update.sh").read_text()
    m = re.search(r"^has_ed25519_user_key\(\) \{\n.*?^\}\n", text, re.S | re.M)
    assert m, "has_ed25519_user_key() not found in update.sh"
    return m.group(0)


def shells() -> list[list[str]]:
    out = [["sh"]]
    hub = os.environ.get("HARMONY_HUB_SH")
    if hub:
        out.append(shlex.split(hub))
    return out


class KeyCheck(unittest.TestCase):
    def answers(self, keys: str | None, shell: list[str]) -> tuple[str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "etc").mkdir()
            (root / "etc/passwd").write_text(f"root:x:0:0:root:{root}/home/root:/bin/sh\n")
            (root / "home/root/.ssh").mkdir(parents=True)
            if keys is not None:
                (root / "home/root/.ssh/authorized_keys").write_text(keys, newline="")
            passwd = f"{root}/etc/passwd"
            deploy = deploy_hub.ED25519_USER_KEY_CHECK.replace("/etc/passwd", passwd)
            update = update_sh_function().replace("/etc/passwd", passwd)
            update += "has_ed25519_user_key && echo yes || echo no\n"
            got = []
            for text in (deploy, update):
                res = subprocess.run([*shell, "-c", text], capture_output=True, text=True, timeout=60)
                got.append(res.stdout.strip())
            return got[0], got[1]

    def test_table(self):
        for shell in shells():
            for label, keys, want in CASES:
                with self.subTest(shell=shell[0], line=label):
                    d, u = self.answers(keys, shell)
                    self.assertEqual(d, want, "deploy_hub.py")
                    self.assertEqual(u, want, "update.sh")

    def test_reported_lines_are_in_the_table(self):
        labels = {c[0]: c[2] for c in CASES}
        for label in REPORTED:
            self.assertEqual(labels[label], "no")

    def test_same_patterns_in_both(self):
        fn = update_sh_function()
        self.assertIn(f"grep -v '{deploy_hub.ED25519_KEY_SKIP}'", fn)
        self.assertIn(f"grep -q '{deploy_hub.ED25519_KEY_LINE}'", fn)
        self.assertIn("grep -v '^[[:blank:]]*#'", fn)
        self.assertIn("grep -v '^[[:blank:]]*#'", deploy_hub.ED25519_USER_KEY_CHECK)

    def test_patterns_need_no_extended_grep(self):
        # the stock Hub BusyBox has no grep -E
        for text in (deploy_hub.ED25519_USER_KEY_CHECK, update_sh_function()):
            self.assertNotIn("grep -E", text)
            self.assertNotIn("egrep", text)


class RootHubKeyChoice(unittest.TestCase):
    """The newest harmony_owner key that is Ed25519, with and without authorize."""

    def choose(self, authorize: bool) -> pathlib.Path:
        with tempfile.TemporaryDirectory() as tmp:
            home = pathlib.Path(tmp)
            ssh = home / ".ssh"
            ssh.mkdir()
            ed = ssh / "harmony_owner_ed25519.pub"
            rsa = ssh / "harmony_owner_rsa.pub"
            ed.write_text(ED + "\n")
            rsa.write_text(RSA + "\n")
            now = time.time()
            os.utime(ed, (now - 100, now - 100))
            os.utime(rsa, (now, now))  # the RSA key is newer
            with mock.patch.object(root_hub_usb.pathlib.Path, "home", return_value=home), \
                 mock.patch("builtins.print"):
                path, _ = root_hub_usb.resolve_ssh_key(authorize=authorize)
            return pathlib.Path(path.name)

    def test_without_authorize(self):
        self.assertEqual(self.choose(False).name, "harmony_owner_ed25519.pub")

    def test_with_authorize(self):
        self.assertEqual(self.choose(True).name, "harmony_owner_ed25519.pub")

    def test_rsa_alone_still_found_without_authorize(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = pathlib.Path(tmp)
            (home / ".ssh").mkdir()
            (home / ".ssh" / "harmony_owner_rsa.pub").write_text(RSA + "\n")
            with mock.patch.object(root_hub_usb.pathlib.Path, "home", return_value=home), \
                 mock.patch("builtins.print"):
                path, line = root_hub_usb.resolve_ssh_key(authorize=False)
            self.assertEqual(path.name, "harmony_owner_rsa.pub")
            self.assertEqual(line, RSA)


if __name__ == "__main__":
    unittest.main(verbosity=2)
