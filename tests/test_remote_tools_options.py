#!/usr/bin/env python3
"""remote/deploy_remote.py and remote/setup_remote.py: no-screen mode, local binaries, ssh config.

No network: deploy_remote runs in-process against the fake remote of
test_deploy_remote_failures.py; setup_remote's device calls are replaced.

- --no-screen reaches the file remote_ui reads on the standard install path too: step 1 of
  setup_remote.py (USB) followed by deploy_remote.py, which now has the option as well.
- deploy_remote.py refuses local binaries that are not ARM before it touches the remote.
- setup_remote.py no longer edits ~/.ssh/config unless --ssh-config is given.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TESTS_DIR = pathlib.Path(__file__).resolve().parent
REPO_DIR = TESTS_DIR.parent
sys.path.insert(0, str(TESTS_DIR))

from test_deploy_remote_failures import FakeRemote, load_tool  # noqa: E402


def elf_header(machine: int) -> bytes:
    return b"\x7fELF\x01\x01\x01" + bytes(11) + machine.to_bytes(2, "little") + bytes(32)


def replay_no_screen(cmds: list[str], data: pathlib.Path) -> pathlib.Path:
    for c in cmds:
        if "no_screen" in c:
            subprocess.run(["sh", "-c", c.replace("/data/", f"{data}/")], check=False, timeout=10)
    return data / "remote_settings.conf"


class DeployRemoteOptions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        self.mod = load_tool()
        files = []
        for name in ("remote_ui", "remote_sync"):
            p = self.base / name
            p.write_bytes(elf_header(40) + name.encode())
            files.append(p)
        self.mod.DEPLOY_FILES = files
        self.mod.RCS_LOCAL_FILE = self.base / "missing"
        self.mod.THEMES_DIR = self.base / "no_themes"
        self.mod.ready_to_go = lambda *a, **k: True

    def tearDown(self):
        self.tmp.cleanup()

    def deploy(self, remote: FakeRemote, *args: str) -> tuple[int, str]:
        self.mod.run_cmd = remote.run_cmd
        out = io.StringIO()
        code = 0
        with mock.patch.object(sys, "argv", ["deploy_remote.py", "--no-sync", *args]), contextlib.redirect_stdout(out):
            try:
                self.mod.main()
            except SystemExit as e:
                code = int(e.code or 0)
        return code, out.getvalue()

    def test_no_screen(self):
        remote = FakeRemote()
        code, out = self.deploy(remote, "--no-screen")
        self.assertEqual(code, 0, out)
        cmds = remote.commands
        ns = next(i for i, c in enumerate(cmds) if "no_screen=1" in c)
        stop = next(i for i, c in enumerate(cmds) if "init.sh stop" in c)
        self.assertLess(ns, stop, "the flag must be in place before remote_ui starts")
        data = self.base / "data"
        data.mkdir()
        (data / "remote_settings.conf").write_text("brightness=3\nno_screen=0\n")
        conf = replay_no_screen(cmds, data)
        lines = conf.read_text().split()
        self.assertIn("brightness=3", lines)
        self.assertEqual([x for x in lines if x.startswith("no_screen=")], ["no_screen=1"])

    def test_without_no_screen(self):
        remote = FakeRemote()
        code, out = self.deploy(remote)
        self.assertEqual(code, 0, out)
        self.assertFalse([c for c in remote.commands if "no_screen" in c])

    def test_refuses_hub_binaries(self):
        self.mod.DEPLOY_FILES[0].write_bytes(elf_header(8))  # MIPS, a Hub build
        remote = FakeRemote()
        code, out = self.deploy(remote)
        self.assertNotEqual(code, 0, out)
        self.assertIn("remote_ui", out)
        self.assertEqual(remote.commands, [], "the remote was contacted with a wrong build")

    def test_default_key_is_the_owner_key(self):
        ssh = self.base / ".ssh"
        ssh.mkdir()
        (ssh / "id_rsa").write_text("other")
        (ssh / "harmony_owner_ed25519").write_text("owner")
        (ssh / "harmony_owner_ed25519.pub").write_text("pub")
        with mock.patch.object(self.mod.pathlib.Path, "home", lambda: self.base), \
             mock.patch.dict(os.environ, {"SSH_KEY": ""}):
            self.assertEqual(self.mod.default_ssh_key(), ssh / "harmony_owner_ed25519")
        remote = FakeRemote()
        seen = []
        orig = remote.run_cmd
        remote.run_cmd = lambda cmd: (seen.append(cmd), orig(cmd))[1]
        with mock.patch.object(self.mod.pathlib.Path, "home", lambda: self.base), \
             mock.patch.dict(os.environ, {"SSH_KEY": ""}):
            self.deploy(remote)
        self.assertTrue(any(str(ssh / "harmony_owner_ed25519") in c for c in seen if c[0] == "ssh"))


class SetupRemoteOptions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        spec = importlib.util.spec_from_file_location("setup_remote_opts", REPO_DIR / "remote" / "setup_remote.py")
        self.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.mod)
        self.ssh: list[str] = []
        key = self.base / "harmony_owner_test"
        key.write_text("private\n")
        (self.base / "harmony_owner_test.pub").write_text("ssh-ed25519 AAAAtest user@test\n")
        self.key = key

    def tearDown(self):
        self.tmp.cleanup()

    def fake_run(self, cmd, *args, **kwargs):
        if cmd and cmd[0] == "ssh":
            self.ssh.append(cmd[-1])
        return subprocess.CompletedProcess(cmd, 0, "" if kwargs.get("text") else b"", "")

    def run_main(self, *args: str) -> str:
        mod = self.mod
        state = mock.MagicMock(mode=mod.UsbMode.USB_HID)
        out = io.StringIO()
        with mock.patch.object(sys, "argv", ["setup_remote.py", "--key", str(self.key), *args]), \
             mock.patch.object(mod, "detect_remote_usb_state", lambda **k: state), \
             mock.patch.object(mod, "print_usb_state_banner", lambda *a, **k: None), \
             mock.patch.object(mod, "step1_enable_usb_debug", lambda *a, **k: True), \
             mock.patch.object(mod, "wait_for_ssh", lambda *a, **k: True), \
             mock.patch.object(mod, "backup_remote", lambda *a, **k: True), \
             mock.patch.object(mod.subprocess, "run", self.fake_run), \
             mock.patch.object(mod.pathlib.Path, "home", lambda: self.base), \
             contextlib.redirect_stdout(out):
            try:
                mod.main()
            except SystemExit:
                pass
        return out.getvalue()

    def test_no_screen_on_step1_path(self):
        self.run_main("--step", "1", "--no-screen", "--no-backup")
        data = self.base / "data"
        data.mkdir()
        conf = replay_no_screen(self.ssh, data)
        self.assertTrue(conf.is_file(), "step 1 dropped --no-screen")
        self.assertIn("no_screen=1", conf.read_text().split())

    def test_ssh_config_left_alone(self):
        out = self.run_main("--step", "1", "--no-backup")
        self.assertFalse((self.base / ".ssh" / "config").exists(), "~/.ssh/config edited without --ssh-config")
        self.assertIn("harmony-remote", out)

    def test_ssh_config_on_request(self):
        self.run_main("--step", "1", "--no-backup", "--ssh-config")
        cfg = (self.base / ".ssh" / "config").read_text()
        self.assertIn("Host 192.168.2.1 harmony-remote", cfg)
        # The project's host key policy: recorded in its own known_hosts file and checked
        self.assertIn("StrictHostKeyChecking accept-new", cfg)
        self.assertIn("UserKnownHostsFile", cfg)
        self.assertNotIn("/dev/null", cfg)


if __name__ == "__main__":
    unittest.main(verbosity=2)
