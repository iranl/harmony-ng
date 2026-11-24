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
         "tools/capture_remote_fb.py", "tools/clean_stock_resources.py", "tools/factory_reset.py",
         "tools/restore_stock.py", "tools/root_hub_usb.py"]
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

WINDOWS = sys.platform == "win32"
IS_ROOT = hasattr(os, "geteuid") and os.geteuid() == 0
ROOT_WRITES = "root can write anywhere"
# The host tools also run on Windows (docs/FIRST_INSTALL_*.md); what Windows cannot do:
NO_FOLDER_MODES = "Windows ignores the mode bits of a folder (NTFS), so a folder cannot be made read-only"
NO_WRITE_ONLY = "Windows has no write-only file: chmod only sets or clears the read-only attribute"
NO_FAKE_SSH = ("the tools start ssh and scp by name, and Windows then starts only an .exe (CreateProcess adds "
               ".exe, never .cmd), so a fake ssh script cannot take the place of ssh.exe")


def kh_option(path: pathlib.Path) -> str:
    """The UserKnownHostsFile option the tools must pass for path: with forward slashes on Windows."""
    text = path.as_posix() if WINDOWS else str(path)
    return f"UserKnownHostsFile={text}"


def locked_folder(test: unittest.TestCase, path: pathlib.Path) -> pathlib.Path:
    """A folder the known_hosts check cannot write in: mode 0555 on POSIX. Windows ignores the
    mode of a folder, so there it is a plain file: nothing can be made under it either."""
    if WINDOWS:
        path.write_text("")
    else:
        path.mkdir()
        path.chmod(0o555)
        test.addCleanup(lambda: path.exists() and path.chmod(0o755))
    return path


def symlink_or_skip(test: unittest.TestCase, link: pathlib.Path, target: pathlib.Path) -> None:
    try:
        link.symlink_to(target)
    except OSError as e:
        if WINDOWS:
            test.skipTest(f"this Windows account cannot make symbolic links (Developer Mode or an "
                          f"administrator is needed): {e}")
        raise


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

    def test_record_host_key_adds_a_missing_type_only(self):
        self.kh.parent.mkdir(parents=True)
        self.kh.write_bytes(b"other ssh-ed25519 AAAAx\n10.0.0.5 ssh-rsa AAAArsa")
        # The Ed25519 key goes next to the recorded RSA one (the file had no final newline)
        self.assertTrue(ssh_hostkeys.record_host_key("root@10.0.0.5", "ssh-ed25519 AAAAnew root@hub"))
        # A recorded key of that type is never replaced
        self.assertFalse(ssh_hostkeys.record_host_key("10.0.0.5", "ssh-ed25519 AAAAother"))
        self.assertFalse(ssh_hostkeys.record_host_key("10.0.0.5", "garbage"))
        self.assertEqual(self.kh.read_bytes(),
                         b"other ssh-ed25519 AAAAx\n10.0.0.5 ssh-rsa AAAArsa\n10.0.0.5 ssh-ed25519 AAAAnew\n")

    def test_options(self):
        opts = ssh_hostkeys.ssh_options("root@HarmonyHub.Local")
        self.assertIn("StrictHostKeyChecking=accept-new", opts)
        self.assertIn(kh_option(self.kh), opts)
        self.assertIn("HashKnownHosts=no", opts)
        # The key is recorded under the name the tool was given, in lower case
        self.assertIn("HostKeyAlias=harmonyhub.local", opts)
        self.assertIn("CheckHostIP=no", opts)
        self.assertTrue(self.kh.parent.is_dir(), "folder of the known_hosts file not created")

    @unittest.skipIf(IS_ROOT, ROOT_WRITES)
    def test_unwritable_file_stops_the_tool(self):
        locked = locked_folder(self, pathlib.Path(self.tmp.name) / "locked")
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
        # No input: Windows OpenSSH waits on an inherited pipe (a run over SSH hung there)
        res = subprocess.run(["ssh", "-G", "-F", str(conf), *ssh_hostkeys.ssh_options("MyHub"), "root@myhub"],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60)
        self.assertEqual(res.returncode, 0, res.stderr)
        cfg = dict(line.split(" ", 1) for line in res.stdout.splitlines() if " " in line)
        self.assertEqual(cfg.get("stricthostkeychecking"), "accept-new")
        # The file the tools gave (ssh -G prints it without quotes; on Windows with forward slashes)
        self.assertEqual(pathlib.Path(cfg.get("userknownhostsfile", "")), self.kh)
        self.assertEqual(cfg.get("hostkeyalias"), "myhub")
        self.assertEqual(cfg.get("checkhostip"), "no")

    @unittest.skipIf(WINDOWS, NO_FOLDER_MODES)
    @unittest.skipIf(IS_ROOT, ROOT_WRITES)
    def test_writable_file_in_read_only_folder(self):
        # Writability is tried, not guessed: ssh can append to a writable file in a folder this
        # account cannot write, so no stop, and --reset-host-key rewrites such a file in place
        ro_dir = pathlib.Path(self.tmp.name) / "ro_dir"
        ro_dir.mkdir()
        (ro_dir / "known_hosts").write_text("")
        ro_dir.chmod(0o555)
        self.addCleanup(lambda: ro_dir.exists() and ro_dir.chmod(0o755))
        with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(ro_dir / "known_hosts")}):
            self.assertIn("StrictHostKeyChecking=accept-new", ssh_hostkeys.ssh_options("192.168.1.20"))
            (ro_dir / "known_hosts").write_bytes(b"192.168.1.20 ssh-ed25519 " + KEY.encode() + b"\r\n# keep\r\n")
            ssh_hostkeys.reset_host("192.168.1.20")
            self.assertEqual((ro_dir / "known_hosts").read_bytes(), b"# keep\r\n")

    def test_error_while_forgetting_is_a_clear_stop(self):
        # Any other error while forgetting: a clear stop, no traceback
        with self.assertRaises(SystemExit) as cm:
            with mock.patch.object(ssh_hostkeys, "forget_host", side_effect=PermissionError(13, "Permission denied")):
                ssh_hostkeys.reset_host("192.168.1.20")
        self.assertIn(ssh_hostkeys.RESET_OPTION, str(cm.exception.code))

    def test_link_into_missing_folder_stops(self):
        # A link to a file in a folder that does not exist: ssh could not write it
        base = pathlib.Path(self.tmp.name)
        link = base / "dangling_known_hosts"
        symlink_or_skip(self, link, base / "missing" / "known_hosts")
        with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(link)}):
            with self.assertRaises(SystemExit) as cm:
                ssh_hostkeys.ssh_options("192.168.1.20")
            self.assertIn(str(link), str(cm.exception.code))

    def test_link_to_file_made_later_passes(self):
        # A link to a file not created yet, in a folder that exists: ssh would create it, so it passes
        base = pathlib.Path(self.tmp.name)
        made_later = base / "later"
        made_later.mkdir()
        link2 = base / "link_to_later"
        symlink_or_skip(self, link2, made_later / "known_hosts")
        with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(link2)}):
            ssh_hostkeys.ssh_options("192.168.1.20")
        self.assertFalse((made_later / "known_hosts").exists(), "the check left the link target behind")

    @unittest.skipIf(WINDOWS, NO_WRITE_ONLY)
    @unittest.skipIf(IS_ROOT, ROOT_WRITES)
    def test_write_only_file_stops(self):
        # A file this account can write but not read: ssh could not check keys in it
        wo = pathlib.Path(self.tmp.name) / "write_only_known_hosts"
        wo.write_text("")
        wo.chmod(0o200)
        with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(wo)}):
            with self.assertRaises(SystemExit):
                ssh_hostkeys.ssh_options("192.168.1.20")

    @unittest.skipUnless(hasattr(os, "mkfifo"), "no FIFOs here")
    def test_fifo_stops_without_hanging(self):
        fifo = pathlib.Path(self.tmp.name) / "fifo_known_hosts"
        os.mkfifo(fifo)
        with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(fifo)}):
            with self.assertRaises(SystemExit) as cm:
                ssh_hostkeys.ssh_options("192.168.1.20")
            self.assertIn("not a regular file", str(cm.exception.code))

    def test_check_leaves_nothing_behind(self):
        fresh = pathlib.Path(self.tmp.name) / "fresh" / "known_hosts"
        with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(fresh)}):
            ssh_hostkeys.ssh_options("192.168.1.20")
        self.assertEqual(list(fresh.parent.iterdir()), [])

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


@unittest.skipIf(WINDOWS, NO_FAKE_SSH)
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
        "capture_remote_fb": (["tools/capture_remote_fb.py", "-o", "BASE/shot.png"], "192.168.2.1"),
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
                    self.assertIn(kh_option(self.kh), c)

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

    @unittest.skipIf(IS_ROOT, ROOT_WRITES)
    def test_unwritable_known_hosts_stops_before_ssh(self):
        locked = locked_folder(self, self.base / "locked")
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
SSHD_PORTS = range(9281, 9300)  # this test group's port range


def free_port() -> int | None:
    """A port of SSHD_PORTS nothing listens on now, or None."""
    import socket
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
class RealOpenSsh(unittest.TestCase):
    """The options against a real OpenSSH server on 127.0.0.1 (no network): the first key is
    recorded under the given name, a changed key is refused with the text the tools look for,
    and forgetting by any case or by an ssh_config alias lets the next connection in."""

    def setUp(self):
        self.port = free_port()
        if self.port is None:
            self.skipTest("no free port in this test's range")
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
        conf.write_text(f"Port {self.port}\nListenAddress 127.0.0.1\nHostKey {self.d / hostkey}\n"
                        f"AuthorizedKeysFile {self.d / 'auth'}\nStrictModes no\nUsePAM no\n"
                        f"PasswordAuthentication no\nPidFile {self.d / 'pid'}\n")
        self.proc = subprocess.Popen([SSHD, "-D", "-e", "-f", str(conf)], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, text=True)
        import socket
        import time
        for _ in range(50):
            # Our sshd first: a listener of someone else must not be taken for it
            if self.proc.poll() is not None:
                self.skipTest(f"sshd does not start here: {self.proc.stderr.read()[:200]}")
            with socket.socket() as s:
                if s.connect_ex(("127.0.0.1", self.port)) == 0:
                    self.check_own_server(hostkey)
                    return
            time.sleep(0.1)
        self.skipTest("sshd did not listen")

    def check_own_server(self, hostkey: str) -> None:
        """Another run may have started its server on the same port first: the server must
        show this test's own host key, else skip."""
        if not shutil.which("ssh-keyscan"):
            return
        res = subprocess.run(["ssh-keyscan", "-p", str(self.port), "-t", "ed25519", "127.0.0.1"],
                             capture_output=True, text=True, timeout=30)
        own = (self.d / f"{hostkey}.pub").read_text().split()[1]
        if own not in res.stdout:
            self.skipTest("another server answers on this test's port")

    def stop(self) -> None:
        if self.proc:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
            self.proc.stderr.close()
            self.proc = None

    def ssh(self, host: str, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(["ssh", *extra, "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-p", str(self.port),
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
        conf.write_text(f"Host myhub\n  HostName 127.0.0.1\n  Port {self.port}\n")
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


class RootHubBackupHint(unittest.TestCase):
    """root_hub_usb.py has rooted the Hub when its stock backup meets a changed key: the hint
    names the short way (backup_hub.py --reset-host-key), not only a new USB run."""

    def test_hint(self):
        sys.path.insert(0, str(REPO_DIR))
        spec = importlib.util.spec_from_file_location("root_hub_usb_hint", REPO_DIR / "tools" / "root_hub_usb.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        with tempfile.TemporaryDirectory() as tmp:
            def changed(cmd, *a, **k):
                return subprocess.CompletedProcess(cmd, 255, "", CHANGED_BANNER)
            err = io.StringIO()
            with mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(pathlib.Path(tmp) / "kh")}), \
                 mock.patch.object(mod, "REPO_DIR", pathlib.Path(tmp)), \
                 mock.patch.object(mod.subprocess, "run", changed), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                self.assertIsNone(mod.backup_stock_partitions("192.168.1.20", pathlib.Path(tmp) / "key"))
        self.assertIn(f"tools/backup_hub.py 192.168.1.20 {ssh_hostkeys.RESET_OPTION}", err.getvalue())


@unittest.skipIf(IS_ROOT, ROOT_WRITES)
class StopBeforeDeviceWork(unittest.TestCase):
    """With a known_hosts file that cannot be written, every tool stops in main() before its
    first USB or device action, so "Nothing was changed" is true."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        locked = locked_folder(self, pathlib.Path(self.tmp.name) / "locked")
        env = mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(locked / "sub" / "known_hosts")})
        env.start()
        self.addCleanup(env.stop)

    def load(self, rel: str, name: str):
        sys.path.insert(0, str(REPO_DIR))
        spec = importlib.util.spec_from_file_location(name, REPO_DIR / rel)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod

    def device_touched(self, *a, **k):
        raise AssertionError("device work started before the known_hosts check")

    def run_main(self, mod, argv):
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                mod.main()
        self.assertIn("SSH host keys cannot be recorded", str(cm.exception.code))

    def test_root_hub_usb(self):
        mod = self.load("tools/root_hub_usb.py", "root_hub_usb_stop")
        with mock.patch.object(mod, "HarmonyHid", self.device_touched):
            self.run_main(mod, ["root_hub_usb.py"])

    def test_restore_stock(self):
        mod = self.load("tools/restore_stock.py", "restore_stock_stop")
        with mock.patch.object(mod, "gain_root_over_usb", self.device_touched), \
             mock.patch.object(mod, "HarmonyHid", self.device_touched), \
             mock.patch.object(mod, "find_latest_stock_backup", self.device_touched):
            self.run_main(mod, ["restore_stock.py", "--force"])

    def test_setup_remote(self):
        mod = self.load("remote/setup_remote.py", "setup_remote_stop")

        class State:
            mode = None
            remediation_cmd = ""
        for step in ([], ["--step", "1"], ["--step", "2"]):
            with self.subTest(step=step), \
                 mock.patch.object(mod, "detect_remote_usb_state", lambda **k: State()), \
                 mock.patch.object(mod, "print_usb_state_banner", lambda *a, **k: None), \
                 mock.patch.object(mod, "step1_enable_usb_debug", self.device_touched), \
                 mock.patch.object(mod, "step2_install_permanent_ssh", self.device_touched), \
                 mock.patch.object(mod, "probe_remote", self.device_touched), \
                 mock.patch.object(mod, "rce", self.device_touched), \
                 mock.patch.object(mod, "ensure_ssh_key", self.device_touched):
                self.run_main(mod, ["setup_remote.py", *step])


class SetupRemoteForgetErrors(unittest.TestCase):
    """setup_remote.py forgets the remote's key after rooting it: an error there must not end
    the setup half-way (the next connection names --reset-host-key if the old key stays)."""

    def test_forget_error_is_reported(self):
        spec = importlib.util.spec_from_file_location("setup_remote_forget", REPO_DIR / "remote" / "setup_remote.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        out = io.StringIO()
        with mock.patch.object(mod.ssh_hostkeys, "forget_host", side_effect=PermissionError(13, "Permission denied")), \
             contextlib.redirect_stdout(out):
            mod.forget_remote_host_key()
        self.assertIn("Could not forget", out.getvalue())


class SetupRemoteBackupErrors(unittest.TestCase):
    """The login check before the remote backup ends in "Backup failed", not a traceback,
    when there is no ssh client or the login hangs."""

    def test_no_ssh_or_timeout(self):
        spec = importlib.util.spec_from_file_location("setup_remote_backup", REPO_DIR / "remote" / "setup_remote.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        errors = [FileNotFoundError(2, "No such file or directory", "ssh"),
                  subprocess.TimeoutExpired(["ssh"], 30)]
        with tempfile.TemporaryDirectory() as tmp:
            for err in errors:
                def fail(*a, _err=err, **k):
                    raise _err
                out = io.StringIO()
                with self.subTest(error=type(err).__name__), \
                     mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(pathlib.Path(tmp) / "kh")}), \
                     mock.patch.object(mod.subprocess, "run", fail), contextlib.redirect_stdout(out):
                    self.assertIsNone(mod.backup_remote(pathlib.Path(tmp) / "key"))
                    self.assertIn("Backup failed", out.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
