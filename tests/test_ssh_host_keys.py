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
        opts = ssh_hostkeys.ssh_options("root@HarmonyHub.Local")
        self.assertIn("StrictHostKeyChecking=accept-new", opts)
        self.assertIn(f'UserKnownHostsFile="{self.kh}"', opts)
        self.assertIn("HashKnownHosts=no", opts)
        # The key is recorded under the name the tool was given, in lower case
        self.assertIn("HostKeyAlias=harmonyhub.local", opts)
        self.assertIn("CheckHostIP=no", opts)
        self.assertTrue(self.kh.parent.is_dir(), "folder of the known_hosts file not created")

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "root can write anywhere")
    def test_unwritable_file_stops_the_tool(self):
        locked = pathlib.Path(self.tmp.name) / "locked"
        locked.mkdir()
        locked.chmod(0o555)
        self.addCleanup(lambda: locked.exists() and locked.chmod(0o755))
        for kh in (locked / "sub" / "known_hosts", locked / "known_hosts"):
            with self.subTest(file=str(kh)), mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(kh)}):
                with self.assertRaises(SystemExit) as cm:
                    ssh_hostkeys.ssh_options("192.168.1.20")
                self.assertIn(str(kh.parent), str(cm.exception.code))
                self.assertIn(ssh_hostkeys.KNOWN_HOSTS_ENV, str(cm.exception.code))
        # A file that exists but cannot be written
        ro = pathlib.Path(self.tmp.name) / "ro_known_hosts"
        ro.write_text("")
        ro.chmod(0o444)
        with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(ro)}):
            with self.assertRaises(SystemExit) as cm:
                ssh_hostkeys.ssh_options("192.168.1.20")
            self.assertIn(str(ro), str(cm.exception.code))

    def test_openssh_reads_the_options(self):
        if not shutil.which("ssh"):
            self.skipTest("no ssh client")
        # A user ssh_config that names the host differently: the options still win
        conf = pathlib.Path(self.tmp.name) / "ssh_config"
        conf.write_text("Host myhub\n  HostName 192.168.1.20\n  Port 2222\n  CheckHostIP yes\n"
                        "  StrictHostKeyChecking no\n  UserKnownHostsFile /dev/null\n")
        res = subprocess.run(["ssh", "-G", "-F", str(conf), *ssh_hostkeys.ssh_options("MyHub"), "root@myhub"],
                             capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        cfg = dict(line.split(" ", 1) for line in res.stdout.splitlines() if " " in line)
        self.assertEqual(cfg.get("stricthostkeychecking"), "accept-new")
        # One file, the path with its space kept whole
        self.assertEqual(cfg.get("userknownhostsfile"), str(self.kh))
        self.assertEqual(cfg.get("hostkeyalias"), "myhub")
        self.assertEqual(cfg.get("checkhostip"), "no")

    def test_forget_by_any_case(self):
        self.kh.parent.mkdir(parents=True)
        self.kh.write_text(f"localhost ssh-ed25519 {KEY}\nharmonyhub.local ssh-ed25519 {KEY}\n")
        self.assertEqual(ssh_hostkeys.forget_host("LocalHost"), 1)
        self.assertEqual(ssh_hostkeys.forget_host("root@HarmonyHub.LOCAL"), 1)
        self.assertEqual(self.kh.read_text(), "")

    def test_forget_keeps_bytes_and_line_ends(self):
        self.kh.parent.mkdir(parents=True)
        other = b"# caf\xe9 not utf-8\r\n192.168.2.10 ssh-ed25519 " + KEY.encode() + b"\r\n"
        self.kh.write_bytes(b"192.168.2.1 ssh-ed25519 " + KEY.encode() + b"\r\n" + other)
        self.assertEqual(ssh_hostkeys.forget_host("192.168.2.1"), 1)
        self.assertEqual(self.kh.read_bytes(), other)

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
                self.assertIn("ssh_hostkeys.ssh_options(", text)
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
                    self.assertIn(f"HostKeyAlias={host}", c)
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

    def test_reset_with_other_case(self):
        self.kh.parent.mkdir(parents=True, exist_ok=True)
        self.kh.write_text(f"harmonyhub.local ssh-ed25519 {KEY}\n")
        self.run_tool([str(REPO_DIR / "build" / "deploy_hub.py"), "HarmonyHub.Local", ssh_hostkeys.RESET_OPTION],
                      mode="down")
        self.assertEqual(self.kh.read_text(), "")
        self.assertIn("HostKeyAlias=harmonyhub.local", self.calls()[0])

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "root can write anywhere")
    def test_unwritable_known_hosts_stops_before_ssh(self):
        locked = self.base / "locked"
        locked.mkdir()
        locked.chmod(0o555)
        self.addCleanup(lambda: locked.exists() and locked.chmod(0o755))
        env = dict(self.env, FAKE_MODE="down", **{ssh_hostkeys.KNOWN_HOSTS_ENV: str(locked / "kh" / "known_hosts")})
        res = subprocess.run([sys.executable, str(REPO_DIR / "build" / "deploy_hub.py"), "192.168.1.20"],
                             capture_output=True, text=True, env=env, timeout=60, cwd=str(self.base))
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("SSH host keys cannot be recorded", res.stderr)
        self.assertIn(ssh_hostkeys.KNOWN_HOSTS_ENV, res.stderr)
        self.assertEqual(self.calls(), [], "ssh ran although no key could be recorded")

    def test_unreachable_is_not_called_a_key_change(self):
        res = self.run_tool(self.prepare(self.CASES["deploy_hub"][0]), mode="down")
        self.assertNotEqual(res.returncode, 0)
        self.assertNotIn(ssh_hostkeys.RESET_OPTION, res.stdout + res.stderr)


SSHD = "/usr/sbin/sshd"
SSHD_PORT = 9281


@unittest.skipUnless(os.path.isfile(SSHD) and shutil.which("ssh") and shutil.which("ssh-keygen")
                     and sys.platform != "win32", "needs OpenSSH sshd, ssh and ssh-keygen")
class RealOpenSsh(unittest.TestCase):
    """The options against a real OpenSSH server on 127.0.0.1 (no network): the first key is
    recorded under the given name, a changed key is refused with the text the tools look for,
    and forgetting by any case or by an ssh_config alias lets the next connection in."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = pathlib.Path(self.tmp.name)
        for name in ("hostkey", "hostkey2", "userkey"):
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(self.d / name)], check=True)
        shutil.copy(self.d / "userkey.pub", self.d / "auth")
        self.kh = self.d / "known_hosts"
        env = mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(self.kh)})
        env.start()
        self.addCleanup(env.stop)
        self.proc = None
        self.addCleanup(self.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def start(self, hostkey: str) -> None:
        self.stop()
        conf = self.d / "sshd_config"
        conf.write_text(f"Port {SSHD_PORT}\nListenAddress 127.0.0.1\nHostKey {self.d / hostkey}\n"
                        f"AuthorizedKeysFile {self.d / 'auth'}\nStrictModes no\nUsePAM no\n"
                        f"PasswordAuthentication no\nPidFile {self.d / 'pid'}\n")
        self.proc = subprocess.Popen([SSHD, "-D", "-e", "-f", str(conf)], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, text=True)
        import socket
        import time
        for _ in range(50):
            with socket.socket() as s:
                if s.connect_ex(("127.0.0.1", SSHD_PORT)) == 0:
                    return
            if self.proc.poll() is not None:
                self.skipTest(f"sshd does not start here: {self.proc.stderr.read()[:200]}")
            time.sleep(0.1)
        self.skipTest("sshd did not listen")

    def stop(self) -> None:
        if self.proc:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
            self.proc.stderr.close()
            self.proc = None

    def ssh(self, host: str, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(["ssh", *extra, "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-p", str(SSHD_PORT),
                               "-i", str(self.d / "userkey"), *ssh_hostkeys.ssh_options(host), host, "true"],
                              capture_output=True, text=True, timeout=30)

    def test_record_refuse_reset(self):
        self.start("hostkey")
        self.assertEqual(self.ssh("LocalHost").returncode, 0)
        self.assertTrue(self.kh.read_text().startswith("localhost ssh-ed25519 "), self.kh.read_text())
        self.start("hostkey2")
        res = self.ssh("localhost")
        self.assertNotEqual(res.returncode, 0)
        self.assertTrue(ssh_hostkeys.is_host_key_failure(res.stderr), res.stderr)
        self.assertEqual(ssh_hostkeys.forget_host("LOCALHOST"), 1)
        self.assertEqual(self.ssh("localhost").returncode, 0)

    def test_ssh_config_alias(self):
        conf = self.d / "ssh_config"
        conf.write_text(f"Host myhub\n  HostName 127.0.0.1\n  Port {SSHD_PORT}\n")
        self.start("hostkey")
        self.assertEqual(self.ssh("myhub", "-F", str(conf)).returncode, 0)
        self.start("hostkey2")
        self.assertNotEqual(self.ssh("myhub", "-F", str(conf)).returncode, 0)
        self.assertEqual(ssh_hostkeys.forget_host("myhub"), 1)
        self.assertEqual(self.ssh("myhub", "-F", str(conf)).returncode, 0)


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
