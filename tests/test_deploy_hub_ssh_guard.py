#!/usr/bin/env python3
"""build/deploy_hub.py installs the Ed25519-only SSH server only when it cannot lock this PC out.

- ED25519_USER_KEY_CHECK (the shell text sent to the Hub) counts a real Ed25519 key line, also one
  with options in front, and not a comment, a missing file, or "ssh-ed25519" in another key's
  comment.
- ed25519_login_problem(): no key, a failed check, and a key of another PC (the Ed25519-only login
  of this PC fails) each give a reason; a key this PC logs in with gives none.
- The login test opens its own connection: with ControlMaster in the ssh config and a shared
  connection already open with this PC's RSA key, a Hub that holds another PC's Ed25519 key is
  still a problem (#267). Run against a real OpenSSH server on 127.0.0.1 (skipped without one).
"""

from __future__ import annotations

import getpass
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))

import deploy_hub  # noqa: E402
import ssh_hostkeys  # noqa: E402

ED = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAiiACbtw7zFBaJ9uICKf3GGoYrHCkZPnK1d7B0o4saR owner\n"
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

    def test_login_test_never_uses_a_shared_connection(self):
        """A ControlMaster in ~/.ssh/config must not carry the login test over an RSA login (#267):
        options on the command line win over the config file"""
        _, seen = self.probe(self.make("yes\n"), 0)
        cmd = seen[0]
        host = cmd.index("root@hub.invalid")
        for opt in ("ControlMaster=no", "ControlPath=none"):
            self.assertIn(opt, cmd)
            self.assertLess(cmd.index(opt), host)
            self.assertEqual(cmd[cmd.index(opt) - 1], "-o")

    def test_this_pc_logs_in(self):
        problem, _ = self.probe(self.make("yes\n"), 0)
        self.assertIsNone(problem)


SSHD = "/usr/sbin/sshd"
SSHD_PORTS = range(9660, 9680)  # this test's port range


def free_port() -> int | None:
    for port in SSHD_PORTS:
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
            except OSError:
                continue
            return port
    return None


@unittest.skipUnless(os.path.isfile(SSHD) and shutil.which("ssh") and shutil.which("ssh-keygen")
                     and sys.platform != "win32", "needs OpenSSH sshd, ssh and ssh-keygen")
class SharedConnection(unittest.TestCase):
    """The Hub holds this PC's RSA key and another PC's Ed25519 key; the user's ssh config shares
    connections (ControlMaster auto, ControlPersist), and an earlier call of the deploy opened one
    with the RSA key. The login test must still find that this PC has no Ed25519 login."""

    def setUp(self):
        self.port = free_port()
        if self.port is None:
            self.skipTest("no free port in this test's range")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.d = pathlib.Path(self.tmp.name)
        for name, kind in (("hostkey", "ed25519"), ("this_pc_rsa", "rsa"), ("other_pc_ed25519", "ed25519")):
            subprocess.run(["ssh-keygen", "-q", "-t", kind, "-N", "", "-f", str(d / name)], check=True)
        (d / "auth").write_text((d / "this_pc_rsa.pub").read_text() + (d / "other_pc_ed25519.pub").read_text())
        # the control socket path must stay short (sun_path)
        self.cm = pathlib.Path(tempfile.mkdtemp(prefix="cm", dir="/tmp" if os.path.isdir("/tmp") else None))
        self.addCleanup(shutil.rmtree, self.cm, True)
        conf = d / "ssh_config"
        conf.write_text(f"Host *\n  ControlMaster auto\n  ControlPath {self.cm}/%C\n  ControlPersist 60\n"
                        "  IdentityAgent none\n  IdentitiesOnly yes\n")
        # "ssh" of the tool = the real one with this config and port (as if in ~/.ssh/config)
        fake = d / "bin"
        fake.mkdir()
        real = shutil.which("ssh")
        (fake / "ssh").write_text(f'#!/bin/sh\nexec "{real}" -F "{conf}" -p {self.port} "$@"\n')
        (fake / "ssh").chmod(0o755)
        env = mock.patch.dict(os.environ, {"PATH": f"{fake}{os.pathsep}{os.environ.get('PATH', '')}",
                                           ssh_hostkeys.KNOWN_HOSTS_ENV: str(d / "known_hosts")})
        env.start()
        self.addCleanup(env.stop)
        sshd_conf = d / "sshd_config"
        sshd_conf.write_text(f"Port {self.port}\nListenAddress 127.0.0.1\nHostKey {d / 'hostkey'}\n"
                             f"AuthorizedKeysFile {d / 'auth'}\nStrictModes no\nUsePAM no\n"
                             f"PasswordAuthentication no\nPidFile {d / 'pid'}\n")
        self.proc = subprocess.Popen([SSHD, "-D", "-e", "-f", str(sshd_conf)], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop)
        for _ in range(50):
            if self.proc.poll() is not None:
                self.skipTest("sshd does not start here")
            with socket.socket() as s:
                if s.connect_ex(("127.0.0.1", self.port)) == 0:
                    break
            time.sleep(0.1)
        else:
            self.skipTest("sshd did not listen")
        self.deployer = deploy_hub.HubDeployer(hub_host="127.0.0.1", hub_user=getpass.getuser(),
                                               ssh_key=str(d / "this_pc_rsa"), no_backup=True)

    def stop(self):
        subprocess.run(["ssh", "-O", "exit", "127.0.0.1"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30)
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()

    def ssh(self, *extra: str) -> int:
        cmd = self.deployer.get_ssh_cmd("true")
        cmd[1:1] = list(extra)
        return subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=60).returncode

    def test_rsa_master_does_not_pass_the_login_test(self):
        # an earlier call of the deploy (RSA key) opens the shared connection
        if self.ssh() != 0:
            self.skipTest("cannot log in to the test sshd here (no login for this account)")
        if self.ssh("-O", "check") != 0:
            self.skipTest("this ssh keeps no shared connection")
        # the scenario is live: an Ed25519-only login over the shared connection passes
        self.assertEqual(self.ssh("-o", "PubkeyAcceptedKeyTypes=ssh-ed25519"), 0)
        self.deployer.run_ssh = lambda cmd: subprocess.CompletedProcess(cmd, 0, "yes\n", "")
        problem = self.deployer.ed25519_login_problem()
        self.assertIsNotNone(problem, "the login test passed over the RSA connection")
        self.assertIn("this PC cannot log in", problem)


if __name__ == "__main__":
    unittest.main(verbosity=2)
