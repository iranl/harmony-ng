#!/usr/bin/env python3
"""Host tools check SSH host keys (tools/ssh_hostkeys.py).

Every host tool that runs ssh or scp records a device's key on first contact
(StrictHostKeyChecking=accept-new) in one project known_hosts file and refuses a changed
key. --reset-host-key forgets the recorded key of the target. A changed key gives a message
that names the option. setup_remote.py forgets the remote's key when it installs SSH (every
rooted remote answers at 192.168.2.1 with a new key).

The tools run with a fake ssh first in PATH (no network); the known_hosts file is a private
temporary file ($HARMONY_KNOWN_HOSTS), never the user's.
"""

from __future__ import annotations

import base64
import contextlib
import hashlib
import hmac
import importlib.util
import io
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "tools"))

import ssh_hostkeys  # noqa: E402

# Every host tool that runs ssh or scp
TOOLS = ["build/deploy_hub.py", "remote/deploy_remote.py", "remote/setup_remote.py", "tools/backup_hub.py",
         "tools/factory_reset.py", "tools/restore_stock.py", "tools/root_hub_usb.py"]
# tools/sync_remote_to_hub.py is removed by another change (#220, legacy leftovers): not covered here
NOT_COVERED = {"tools/sync_remote_to_hub.py"}

# What OpenSSH prints when the recorded key differs (BatchMode, StrictHostKeyChecking=accept-new)
CHANGED_BANNER = """@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
Host key for 192.168.1.20 has changed and you have requested strict checking.
Host key verification failed.
"""

FAKE_SSH = """#!/bin/sh
printf '%s\\n' "$*" >> "$FAKE_DIR/log"
case "$FAKE_MODE" in
  changed) cat "$FAKE_DIR/banner" >&2; exit 255 ;;
  *) echo "ssh: connect to host device port 22: Connection refused" >&2; exit 255 ;;
esac
"""

KEY = "AAAAC3NzaC1lZDI1NTE5AAAAIOTESTKEYxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"


def hashed_name(name: str, salt: bytes = b"0123456789abcdefghij") -> str:
    mac = hmac.new(salt, name.encode(), hashlib.sha1).digest()
    return f"|1|{base64.b64encode(salt).decode()}|{base64.b64encode(mac).decode()}"


class HelperTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.kh = pathlib.Path(self.tmp.name) / "dir with space" / "known_hosts"
        env = mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(self.kh)})
        env.start()
        self.addCleanup(env.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def test_options(self):
        opts = ssh_hostkeys.ssh_options()
        self.assertIn("StrictHostKeyChecking=accept-new", opts)
        self.assertIn(f'UserKnownHostsFile="{self.kh}"', opts)
        self.assertIn("HashKnownHosts=no", opts)
        self.assertTrue(self.kh.parent.is_dir(), "folder of the known_hosts file not created")

    def test_openssh_reads_the_options(self):
        if not shutil.which("ssh"):
            self.skipTest("no ssh client")
        res = subprocess.run(["ssh", "-G", *ssh_hostkeys.ssh_options(), "root@192.168.1.20"],
                             capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        cfg = dict(line.split(" ", 1) for line in res.stdout.splitlines() if " " in line)
        self.assertEqual(cfg.get("stricthostkeychecking"), "accept-new")
        # One file, the path with its space kept whole
        self.assertEqual(cfg.get("userknownhostsfile"), str(self.kh))

    def test_forget_host(self):
        self.kh.parent.mkdir(parents=True)
        self.kh.write_text("\n".join([
            "# comment 192.168.2.1",
            f"192.168.2.1 ssh-ed25519 {KEY}",
            f"192.168.2.1 ssh-rsa {KEY}",
            f"192.168.1.20,192.168.2.1 ssh-ed25519 {KEY}",
            f"[192.168.2.1]:2222 ssh-ed25519 {KEY}",
            f"{hashed_name('192.168.2.1')} ssh-ed25519 {KEY}",
            f"{hashed_name('192.168.1.20')} ssh-ed25519 {KEY}",
            f"@revoked 192.168.2.1 ssh-ed25519 {KEY}",
            f"192.168.2.10 ssh-ed25519 {KEY}",
        ]) + "\n")
        self.assertEqual(ssh_hostkeys.forget_host("192.168.2.1"), 4)
        left = self.kh.read_text().splitlines()
        self.assertEqual(left, [
            "# comment 192.168.2.1",
            f"[192.168.2.1]:2222 ssh-ed25519 {KEY}",
            f"{hashed_name('192.168.1.20')} ssh-ed25519 {KEY}",
            f"@revoked 192.168.2.1 ssh-ed25519 {KEY}",
            f"192.168.2.10 ssh-ed25519 {KEY}",
        ])
        self.assertEqual(ssh_hostkeys.forget_host("192.168.2.1"), 0)

    def test_forget_without_file(self):
        self.assertEqual(ssh_hostkeys.forget_host("192.168.2.1"), 0)
        self.assertFalse(self.kh.exists())

    def test_failure_text(self):
        self.assertTrue(ssh_hostkeys.is_host_key_failure(CHANGED_BANNER))
        self.assertTrue(ssh_hostkeys.is_host_key_failure(CHANGED_BANNER.encode()))
        self.assertFalse(ssh_hostkeys.is_host_key_failure("ssh: connect to host x port 22: Connection refused"))
        self.assertFalse(ssh_hostkeys.is_host_key_failure(None))


class ToolSources(unittest.TestCase):
    def test_no_tool_turns_checking_off(self):
        for rel in TOOLS:
            text = (REPO_DIR / rel).read_text(encoding="utf-8")
            with self.subTest(tool=rel):
                self.assertNotIn("StrictHostKeyChecking=no", text)
                self.assertNotIn("UserKnownHostsFile=/dev/null", text)
                self.assertNotRegex(text, r'UserKnownHostsFile=" \+')
                self.assertIn("ssh_hostkeys.ssh_options()", text)
                self.assertIn("ssh_hostkeys.add_reset_option(", text)

    def test_every_ssh_tool_is_listed(self):
        found = set()
        for top in ("build", "remote", "tools", "scripts"):
            for path in sorted((REPO_DIR / top).rglob("*.py")):
                rel = path.relative_to(REPO_DIR).as_posix()
                if "/node_modules/" in rel or "/toolchains/" in rel:
                    continue
                text = path.read_text(encoding="utf-8", errors="replace")
                if re.search(r'\[\s*"(ssh|scp)"', text):
                    found.add(rel)
        self.assertEqual(found - set(TOOLS) - NOT_COVERED, set(), "a tool runs ssh without the shared host key options")

    def test_release_bundles_ship_the_helper(self):
        text = (REPO_DIR / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        for staging in ("staging_hub/tools/", "staging_remote/tools/"):
            line = next(ln for ln in text.splitlines() if ln.rstrip().endswith(staging) and "cp -f" in ln)
            self.assertIn("tools/ssh_hostkeys.py", line, staging)


class ToolRuns(unittest.TestCase):
    """The tools against a fake ssh: options used, changed key reported, reset option."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        fake = self.base / "bin"
        fake.mkdir()
        for name in ("ssh", "scp"):
            (fake / name).write_text(FAKE_SSH)
            (fake / name).chmod(0o755)
        (self.base / "banner").write_text(CHANGED_BANNER)
        self.kh = self.base / "kh" / "known_hosts"
        self.env = dict(os.environ, PATH=f"{fake}{os.pathsep}{os.environ['PATH']}", FAKE_DIR=str(self.base),
                        HOME=str(self.base), SSH_KEY="", HUB_IP="",
                        **{ssh_hostkeys.KNOWN_HOSTS_ENV: str(self.kh)})

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, args, mode="changed"):
        env = dict(self.env, FAKE_MODE=mode)
        return subprocess.run([sys.executable] + args, capture_output=True, text=True, env=env, timeout=60,
                              cwd=str(self.base), stdin=subprocess.DEVNULL)

    def calls(self):
        log = self.base / "log"
        return log.read_text().splitlines() if log.exists() else []

    CASES = {
        "deploy_hub": (["build/deploy_hub.py", "192.168.1.20"], "192.168.1.20"),
        "backup_hub": (["tools/backup_hub.py", "192.168.1.20", "--backup-dir", "BASE/backups"], "192.168.1.20"),
        "factory_reset hub": (["tools/factory_reset.py", "--target", "hub", "--hub-ip", "192.168.1.20", "--force"],
                              "192.168.1.20"),
        "factory_reset remote": (["tools/factory_reset.py", "--target", "remote", "--force"], "192.168.2.1"),
        "restore_stock": (["tools/restore_stock.py", "--backup", "BASE/stock.tar", "--force", "--skip-usb-root",
                           "--hub-ip", "192.168.1.20", "--pubkey", "BASE/owner.pub"], "192.168.1.20"),
        "deploy_remote": (["remote/deploy_remote.py", "--check"], "192.168.2.1"),
    }

    def prepare(self, args):
        out = []
        for a in args:
            a = a.replace("BASE", str(self.base))
            out.append(str(REPO_DIR / a) if a.endswith(".py") and not a.startswith(str(self.base)) else a)
        # restore_stock: a valid (small) backup archive and an owner key
        import tarfile
        with tarfile.open(self.base / "stock.tar", "w") as t:
            for name in ("cache/x", "data/resources/DeviceList.json"):
                info = tarfile.TarInfo(name)
                info.size = 0
                t.addfile(info, io.BytesIO(b""))
        (self.base / "owner.pub").write_text("ssh-ed25519 AAAAtest owner\n")
        (self.base / "owner").write_text("private\n")
        return out

    def test_changed_key_is_reported(self):
        for name, (args, host) in self.CASES.items():
            with self.subTest(tool=name):
                (self.base / "log").unlink(missing_ok=True)
                res = self.run_tool(self.prepare(args))
                text = res.stdout + res.stderr
                self.assertNotEqual(res.returncode, 0, text)
                self.assertIn(ssh_hostkeys.RESET_OPTION, text, f"{name} did not name the reset option:\n{text}")
                self.assertIn(f"SSH host key of {host}", text)
                calls = self.calls()
                self.assertTrue(calls, f"{name} ran no ssh")
                for c in calls:
                    self.assertIn("StrictHostKeyChecking=accept-new", c)
                    self.assertIn(f'UserKnownHostsFile="{self.kh}"', c)

    def test_reset_option_forgets_the_target_only(self):
        for name, (args, host) in self.CASES.items():
            with self.subTest(tool=name):
                self.kh.parent.mkdir(parents=True, exist_ok=True)
                self.kh.write_text(f"{host} ssh-ed25519 {KEY}\n192.168.9.9 ssh-ed25519 {KEY}\n")
                res = self.run_tool(self.prepare(args) + [ssh_hostkeys.RESET_OPTION], mode="down")
                left = self.kh.read_text()
                self.assertNotIn(f"{host} ssh", left, f"{name}: {res.stdout}{res.stderr}")
                self.assertIn("192.168.9.9 ssh", left)

    def test_unreachable_is_not_called_a_key_change(self):
        res = self.run_tool(self.prepare(self.CASES["deploy_hub"][0]), mode="down")
        self.assertNotEqual(res.returncode, 0)
        self.assertNotIn(ssh_hostkeys.RESET_OPTION, res.stdout + res.stderr)


class SetupRemoteForgetsOnInstall(unittest.TestCase):
    """setup_remote.py step 2 installs SSH (a new key): the old key of 192.168.2.1 is forgotten
    before its first connection, which then records the new one."""

    def test_step2(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            kh = base / "known_hosts"
            kh.write_text(f"192.168.2.1 ssh-ed25519 {KEY}\n192.168.1.20 ssh-ed25519 {KEY}\n")
            pub = base / "key.pub"
            pub.write_text("ssh-ed25519 AAAAtest user@test\n")
            spec = importlib.util.spec_from_file_location("setup_remote_hostkeys", REPO_DIR / "remote" / "setup_remote.py")
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            sizes = {"/tmp/tinysz.json": str(len(mod.TINY_PATH.read_bytes())),
                     "/tmp/dbsz.json": str(len(mod.DROPBEAR_PATH.read_bytes()))}
            seen: list[list[str]] = []

            def fake_run(cmd, *a, **k):
                if cmd and cmd[0] == "ssh" and not seen:
                    seen.append(list(cmd))
                    # The first connection must not meet the old key
                    self.assertNotIn("192.168.2.1 ssh", kh.read_text())
                return subprocess.CompletedProcess(cmd, 0, "Linux juniper armv5tejl\n" if k.get("text") else b"", "")

            with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(kh)}), \
                 mock.patch.object(mod, "rce", lambda *a, **k: None), \
                 mock.patch.object(mod, "read_json_val", lambda f, key, timeout=5: sizes[f]), \
                 mock.patch.object(mod.time, "sleep", lambda s: None), \
                 mock.patch.object(mod.subprocess, "run", fake_run), \
                 contextlib.redirect_stdout(io.StringIO()):
                mod.step2_install_permanent_ssh(pub, no_backup=True)
            self.assertTrue(seen, "no ssh call")
            self.assertIn("StrictHostKeyChecking=accept-new", seen[0])
            self.assertEqual(kh.read_text(), f"192.168.1.20 ssh-ed25519 {KEY}\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
