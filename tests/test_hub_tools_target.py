#!/usr/bin/env python3
"""tools/backup_hub.py and build/deploy_hub.py: right device, real backup, per-Hub rotation.

No network: backup_hub runs with a fake ssh first in PATH; deploy_hub runs in-process with
its ssh calls recorded.

- Both tools refuse a target that is not a MIPS board (the remote answers on the same key).
- deploy_hub refuses local binaries that are not MIPS, and stops before touching the Hub
  when the backup did not work (unless --no-backup).
- backup_hub reports failure when no tar can be used, saves the Wi-Fi settings, the SSH
  host keys and the stock boot script copy, names files by Hub, and rotates each Hub's
  backups on its own, keeping one per week beyond the newest five.
- In a release bundle deploy_hub takes VERSION from the bundle, not from the folder above.
"""

from __future__ import annotations

import contextlib
import datetime
import importlib.util
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))
sys.path.insert(0, str(REPO_DIR / "tools"))

import backup_hub  # noqa: E402
import deploy_hub  # noqa: E402

FAKE_SSH = """#!/bin/sh
for last; do :; done
printf '%s\\n' "$last" >> "$FAKE_DIR/log"
case "$last" in
  *"-cf -"*) cat "$FAKE_TAR"; exit 0 ;;
  *"--help"*) exit "${FAKE_TAR_HELP_RC:-0}" ;;
  "uname -m") echo "$FAKE_MACHINE" ;;
  *ath0/address*) echo "$FAKE_MAC" ;;
  *"walk()"*) echo "2 99914b932bd37a50b983c5e7c90ae93b /data/resources/DeviceList.json" ;;  # stock files on the Hub
  *) exit 0 ;;
esac
"""


def elf_header(machine: int) -> bytes:
    # 32-bit big-endian ELF header with the given e_machine (enough for the check)
    return b"\x7fELF\x01\x02\x01" + bytes(11) + machine.to_bytes(2, "big") + bytes(32)


class BackupHubTarget(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        (base / "bin").mkdir()
        ssh = base / "bin" / "ssh"
        ssh.write_text(FAKE_SSH)
        ssh.chmod(0o755)
        self.tar = base / "hub.tar"
        with tarfile.open(self.tar, "w") as tf:
            info = tarfile.TarInfo("data/harmony/config/devices.json")
            info.size = 2
            tf.addfile(info, io.BytesIO(b"{}"))
        self.base = base
        self.backups = base / "backups"
        self.env = dict(os.environ)
        os.environ.update(PATH=f"{base / 'bin'}:{os.environ['PATH']}", FAKE_DIR=str(base), FAKE_TAR=str(self.tar),
                          FAKE_MACHINE="mips", FAKE_MAC="00:11:22:33:44:55")

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.env)
        self.tmp.cleanup()

    def run_backup(self) -> bool:
        with contextlib.redirect_stdout(io.StringIO()):
            return backup_hub.backup_hub("hub.invalid", ssh_key=None, backup_dir=self.backups, repo_root=self.base)

    def commands(self) -> list[str]:
        return (self.base / "log").read_text().splitlines()

    def test_refuses_the_remote(self):
        os.environ["FAKE_MACHINE"] = "armv5tejl"
        self.assertFalse(self.run_backup())
        self.assertFalse([c for c in self.commands() if "-cf -" in c], "a backup was taken of the remote")
        self.assertEqual(list(self.backups.glob("*.tar")), [])

    def test_no_tar_is_a_failure(self):
        os.environ["FAKE_TAR_HELP_RC"] = "1"  # no tar applet, and no local busybox to stage
        self.assertFalse(self.run_backup(), "a skipped backup was reported as done")

    def test_saves_wifi_host_keys_and_stock_boot_script(self):
        self.assertTrue(self.run_backup())
        rolling = [c for c in self.commands() if "-cf -" in c and "/data/harmony" in c]
        self.assertTrue(rolling)
        for path in ("/etc/wpa_supplicant.conf", "/etc/dropbear", "/data/stock_backup"):
            self.assertIn(path, rolling[0])

    def test_rotation_per_hub(self):
        self.backups.mkdir()
        other = []
        for i in range(7):
            f = self.backups / f"hub_backup_aabbccddeeff_2026010{i}_120000.tar"
            f.write_bytes(self.tar.read_bytes())
            other.append(f)
        legacy = self.backups / "hub_backup_20260101_120000.tar"
        legacy.write_bytes(self.tar.read_bytes())
        for _ in range(7):
            self.assertTrue(self.run_backup())
            time.sleep(1.05)  # one file name per second
        mine = list(self.backups.glob("hub_backup_001122334455_*.tar"))
        self.assertEqual(len(mine), backup_hub.KEEP_RECENT)  # all in this week: the 5 newest only
        self.assertTrue(all(f.exists() for f in other), "backups of another Hub were deleted")
        self.assertTrue(legacy.exists())
        self.assertTrue((self.backups / "golden_hub_backup_001122334455.tar").is_file())

    def test_legacy_golden_backup_is_kept(self):
        """A golden_hub_backup.tar from before the per-Hub names is the stock backup: no new
        golden backup (of today's state) is written next to it, and it is not touched."""
        self.backups.mkdir()
        legacy = self.backups / "golden_hub_backup.tar"
        legacy.write_bytes(b"stock state")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertTrue(backup_hub.backup_hub("hub.invalid", ssh_key=None, backup_dir=self.backups,
                                                  repo_root=self.base))
        self.assertIn("legacy golden backup kept: golden_hub_backup.tar", out.getvalue())
        self.assertEqual(sorted(p.name for p in self.backups.glob("golden_hub_backup*")), ["golden_hub_backup.tar"])
        self.assertEqual(legacy.read_bytes(), b"stock state")
        self.assertEqual(len(list(self.backups.glob("hub_backup_001122334455_*.tar"))), 1, "the rolling backup was not made")

    def test_weekly_generations(self):
        now = time.time()
        files = []
        for days in range(0, 120, 3):  # one backup every 3 days for 4 months
            f = self.backups / f"hub_backup_x_{days:03d}.tar"
            f.parent.mkdir(exist_ok=True)
            f.write_bytes(b"x")
            os.utime(f, (now - days * 86400, now - days * 86400))
            files.append(f)
        removed = set(backup_hub.backups_to_remove(files))
        kept = [f for f in files if f not in removed]
        self.assertEqual(kept[:backup_hub.KEEP_RECENT], files[:backup_hub.KEEP_RECENT])
        older = kept[backup_hub.KEEP_RECENT:]
        weeks = {datetime.date.fromtimestamp(f.stat().st_mtime).isocalendar()[:2] for f in older}
        self.assertEqual(len(older), backup_hub.KEEP_WEEKS)
        self.assertEqual(len(weeks), backup_hub.KEEP_WEEKS, "one kept backup per week")

    def test_no_extra_backup_in_a_recent_week(self):
        """The weekly ones are for the weeks before the newest five: a week that already holds
        one of them gets no sixth file."""
        wed = datetime.datetime(2026, 11, 11, 12, 0).timestamp()  # a Wednesday
        files = []
        stamps = [wed - h * 3600 for h in range(7)] + [wed - d * 86400 for d in (7, 14, 21)]
        for i, ts in enumerate(stamps):
            f = self.backups / f"hub_backup_y_{i:02d}.tar"
            f.parent.mkdir(exist_ok=True)
            f.write_bytes(b"x")
            os.utime(f, (ts, ts))
            files.append(f)
        removed = set(backup_hub.backups_to_remove(files))
        self.assertEqual(removed, set(files[5:7]), "older backups of the newest five's week were kept")


class DeployHubTarget(unittest.TestCase):
    def make(self, machine: str = "mips", no_backup: bool = True):
        d = deploy_hub.HubDeployer(hub_host="hub.invalid", ssh_key="/nonexistent", no_backup=no_backup)
        self.ssh: list[str] = []

        def fake_ssh(cmd: str) -> subprocess.CompletedProcess[str]:
            self.ssh.append(cmd)
            if cmd == "uname -m":
                return subprocess.CompletedProcess(cmd, 0, machine + "\n", "")
            return subprocess.CompletedProcess(cmd, 0, "", "")

        d.run_ssh = fake_ssh
        d.deploy_file_if_changed = lambda *a, **k: True
        return d

    def deploy(self, d) -> bool:
        out = io.StringIO()
        with mock.patch.object(deploy_hub.subprocess, "run",
                               side_effect=lambda cmd, *a, **k: subprocess.CompletedProcess(cmd, 1, "", "")), \
             mock.patch.object(deploy_hub.time, "sleep", lambda s: None), \
             mock.patch("builtins.input", return_value="n"), \
             contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            return d.deploy()

    def stopped(self) -> bool:
        return any("init.sh stop" in c for c in self.ssh)

    def test_refuses_the_remote(self):
        d = self.make(machine="armv5tejl")
        self.assertFalse(self.deploy(d))
        self.assertEqual(self.ssh, ["uname -m"], "the tool changed something on a device that is not a Hub")

    def test_refuses_binaries_of_another_architecture(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = self.make()
            d.bin_dir = pathlib.Path(tmp)
            (d.bin_dir / "hub_daemon").write_bytes(elf_header(40))  # ARM
            (d.bin_dir / "hub_rf").write_bytes(elf_header(8))  # MIPS
            self.assertEqual(d.wrong_arch_binaries(), ["hub_daemon"])
            self.assertFalse(self.deploy(d))
            self.assertFalse(self.stopped())

    def test_failed_backup_stops_before_the_hub_is_touched(self):
        d = self.make(no_backup=False)
        with mock.patch.object(backup_hub, "backup_hub", return_value=False):
            self.assertFalse(self.deploy(d))
        self.assertFalse(self.stopped(), "services stopped although no backup was made")

    def test_backup_exception_stops_too(self):
        d = self.make(no_backup=False)
        with mock.patch.object(backup_hub, "backup_hub", side_effect=OSError("disk full")):
            self.assertFalse(self.deploy(d))
        self.assertFalse(self.stopped())


class DeployHubBundleLayout(unittest.TestCase):
    def test_version_from_the_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent = pathlib.Path(tmp)
            bundle = parent / "harmony-hub"
            (bundle / "bin").mkdir(parents=True)
            shutil.copy(REPO_DIR / "build" / "deploy_hub.py", bundle / "deploy_hub.py")
            (bundle / "VERSION").write_text("9.9.9\n")
            (parent / "VERSION").write_text("1.0.0\n")  # an unrelated file in the download folder
            (parent / "build").mkdir()
            (parent / "build" / "icons.tar.gz").write_bytes(b"other")
            (bundle / "icons.tar.gz").write_bytes(b"mine")
            spec = importlib.util.spec_from_file_location("deploy_hub_bundle", bundle / "deploy_hub.py")
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            d = mod.HubDeployer(hub_host="hub.invalid", ssh_key="/nonexistent")
            self.assertEqual(d.version_file, bundle / "VERSION")
            self.assertEqual(d.icons_tar, bundle / "icons.tar.gz")


class HubToolKeysAndPasswords(unittest.TestCase):
    def test_private_key_next_to_public_key(self):
        import root_hub_usb
        self.assertEqual(root_hub_usb.private_key_path(pathlib.Path("/k/id.backup.pub")), pathlib.Path("/k/id.backup"))
        self.assertEqual(root_hub_usb.private_key_path(pathlib.Path("/k/hub.key.pub")), pathlib.Path("/k/hub.key"))
        with self.assertRaises(ValueError):
            root_hub_usb.private_key_path(pathlib.Path("/k/mykey.pub.txt"))

    def test_wifi_password_from_environment(self):
        import root_hub_usb
        args = mock.Mock(wifi_pass=None, wifi_enc="WPA2-PSK")
        with mock.patch.dict(os.environ, {root_hub_usb.WIFI_PASS_ENV: "placeholder"}), \
             mock.patch("getpass.getpass", side_effect=AssertionError("asked although the variable is set")):
            self.assertEqual(root_hub_usb.wifi_password(args, "net"), "placeholder")
        args = mock.Mock(wifi_pass="placeholder", wifi_enc="WPA2-PSK")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(root_hub_usb.wifi_password(args, "net"), "placeholder")
        self.assertIn("shell history", out.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
