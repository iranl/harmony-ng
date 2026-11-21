#!/usr/bin/env python3
"""tools/restore_stock.py against a fake Hub on the host.

restore_stock_configuration() runs unchanged; its ssh calls run with sh in a
temporary directory that stands for the Hub's root (device paths moved under
it). killall and reboot are only recorded. The fake BusyBox hands "tar" to the
host's tar. No network.
"""

from __future__ import annotations

import io
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools import restore_stock  # noqa: E402

REAL_RUN = subprocess.run
REAL_POPEN = subprocess.Popen

FAKE_BUSYBOX = '#!/bin/sh\ncase "$1" in\n  tar) shift; exec tar "$@" ;;\n  sync) exit 0 ;;\nesac\nexit 1\n'
# A tar that fails at once without reading its input
FAILING_BUSYBOX = '#!/bin/sh\ncase "$1" in\n  tar) echo "tar: invalid option" >&2; exit 1 ;;\n  sync) exit 0 ;;\nesac\nexit 1\n'


def make_backup(path: pathlib.Path) -> None:
    with tarfile.open(path, "w") as tar:
        for name, data in (("cache/stock_cache_file", b"cache\n"), ("data/stock_data_file", b"data\n")):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        # Padding so the archive is not tiny
        info = tarfile.TarInfo("data/padding")
        info.size = 4096
        tar.addfile(info, io.BytesIO(b"\0" * 4096))


class FakeHub:
    def __init__(self, root: pathlib.Path):
        self.root = root
        self.commands: list[str] = []
        self.machine = "mips"
        self.mac = "00:11:22:33:44:55\n"

    def map(self, cmd: str) -> str:
        # POSIX form: sh would read the backslashes of a Windows path as escapes
        root = self.root.as_posix()
        cmd = re.sub(r"(?<![\w./-])/(cache|data|tmp|usr|etc)/", lambda m: f"{root}/{m.group(1)}/", cmd)
        return cmd.replace("-C /", f"-C {root}/")

    def unmap(self, out):
        if isinstance(out, str):
            return out.replace(self.root.as_posix(), "")
        return out

    def run(self, argv, *args, **kwargs):
        if not argv or argv[0] != "ssh":
            return REAL_RUN(argv, *args, **kwargs)
        cmd = argv[-1]
        self.commands.append(cmd)
        if cmd == "uname -m":
            return subprocess.CompletedProcess(argv, 0, self.machine + "\n", "")
        if "ath0/address" in cmd:
            return subprocess.CompletedProcess(argv, 0 if self.mac else 1, self.mac, "")
        if cmd.startswith("killall") or cmd.startswith("reboot"):
            return subprocess.CompletedProcess(argv, 0, "", "")
        res = REAL_RUN(["sh", "-c", self.map(cmd)], *args, **kwargs)
        res.stdout = self.unmap(res.stdout)
        return res

    def popen(self, argv, *args, **kwargs):
        # subprocess.run() itself goes through Popen: only ssh is the Hub
        if not argv or argv[0] != "ssh":
            return REAL_POPEN(argv, *args, **kwargs)
        cmd = argv[-1]
        self.commands.append(cmd)
        return REAL_POPEN(["sh", "-c", self.map(cmd)], *args, **kwargs)


class RestoreStock(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.root = base / "hub"
        for d in ("cache/bin", "data/harmony", "tmp", "usr/sbin", "etc"):
            (self.root / d).mkdir(parents=True)
        (self.root / "cache" / "www_leftover").write_text("old\n")
        # The stock files on the Hub (the backups made here hold none)
        (self.root / "data" / "resources").mkdir()
        (self.root / "data" / "resources" / "DeviceList.json").write_text("{}\n")
        self.backups = base / "backups"
        self.backups.mkdir()
        dir_patch = patch.object(restore_stock, "BACKUP_DIR", self.backups)
        dir_patch.start()
        self.addCleanup(dir_patch.stop)
        self.backup = base / "stock_hub_backup_latest.tar"
        make_backup(self.backup)
        self.hub = FakeHub(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def install_busybox(self, rel: str, script: str = FAKE_BUSYBOX) -> None:
        bb = self.root / rel
        bb.parent.mkdir(parents=True, exist_ok=True)
        bb.write_text(script)
        bb.chmod(0o755)

    def restore(self, pure_stock: bool = False, stock_copy: pathlib.Path | None = None) -> None:
        try:
            with tarfile.open(self.backup, "r:*") as tar:
                members = tar.getnames()
        except (tarfile.TarError, OSError, EOFError):
            members = []
        with patch.object(restore_stock.subprocess, "run", side_effect=self.hub.run), \
             patch.object(restore_stock.subprocess, "Popen", side_effect=self.hub.popen), \
             patch.object(restore_stock.time, "sleep"):
            restore_stock.restore_stock_configuration("hub.invalid", None, self.backup, pure_stock, members,
                                                      stock_copy)

    def rebooted(self) -> bool:
        return any(c.startswith("reboot") for c in self.hub.commands)

    def test_busybox_in_cache(self):
        """HarmonyNG Hub: BusyBox in /cache/bin, which the restore empties."""
        self.install_busybox("cache/bin/busybox")
        self.restore()
        self.assertTrue((self.root / "cache" / "stock_cache_file").is_file())
        self.assertTrue((self.root / "data" / "stock_data_file").is_file())
        self.assertFalse((self.root / "cache" / "www_leftover").exists())
        self.assertTrue(self.rebooted())

    def test_busybox_in_data(self):
        self.install_busybox("data/harmony/bin/busybox")
        self.restore()
        self.assertTrue((self.root / "cache" / "stock_cache_file").is_file())
        self.assertTrue((self.root / "data" / "stock_data_file").is_file())
        self.assertTrue(self.rebooted())

    def test_failed_extraction_stops_before_reboot(self):
        self.install_busybox("cache/bin/busybox")
        # Cut archive: tar fails
        data = self.backup.read_bytes()
        self.backup.write_bytes(data[:700])
        with self.assertRaises(RuntimeError):
            self.restore(pure_stock=True)
        self.assertFalse(self.rebooted())
        self.assertFalse(any("dropbear" in c for c in self.hub.commands))

    def test_tar_failing_at_once_reports_its_error(self):
        """tar exits before reading the archive: the restore stops with tar's exit code and
        message, not with a broken pipe."""
        self.install_busybox("cache/bin/busybox", FAILING_BUSYBOX)
        # Larger than a pipe buffer, so the copy is still writing when tar has gone
        with tarfile.open(self.backup, "a") as tar:
            info = tarfile.TarInfo("data/big")
            info.size = 1 << 20
            tar.addfile(info, io.BytesIO(b"\0" * info.size))
        with self.assertRaises(RuntimeError) as ctx:
            self.restore(pure_stock=True)
        self.assertIn("tar exit code 1", str(ctx.exception))
        self.assertIn("invalid option", str(ctx.exception))
        self.assertFalse(self.rebooted())
        self.assertFalse(any("dropbear" in c for c in self.hub.commands))

    def emptied_hub(self) -> None:
        for f in (self.root / "data" / "resources").iterdir():
            f.unlink()

    def test_emptied_hub_without_saved_stock_files_is_refused(self):
        """The stock files were removed and the archive has none: stop before anything changes."""
        self.install_busybox("cache/bin/busybox")
        self.emptied_hub()
        with self.assertRaises(RuntimeError) as ctx:
            self.restore()
        self.assertIn("stock_resources_001122334455.tar", str(ctx.exception))
        self.assertTrue((self.root / "cache" / "www_leftover").exists())
        self.assertFalse(any(c.startswith("killall") for c in self.hub.commands))

    STOCK_DATA = b'{"devices": []}\n'

    def stock_copy(self, name: str = "stock_resources_001122334455.tar") -> pathlib.Path:
        saved = self.backups / name
        with tarfile.open(saved, "w") as tar:
            info = tarfile.TarInfo("data/resources/DeviceList.json")
            info.size = len(self.STOCK_DATA)
            tar.addfile(info, io.BytesIO(self.STOCK_DATA))
        return saved

    def test_emptied_hub_gets_the_saved_stock_files_back(self):
        self.install_busybox("cache/bin/busybox")
        self.emptied_hub()
        self.stock_copy()
        self.restore()
        self.assertEqual((self.root / "data" / "resources" / "DeviceList.json").read_bytes(), self.STOCK_DATA)
        self.assertTrue((self.root / "data" / "stock_data_file").is_file())
        self.assertTrue(self.rebooted())

    def config_backup(self) -> None:
        """A rolling backup (hub_backup_<hub>_<time>.tar): HarmonyNG's own data, no stock files"""
        with tarfile.open(self.backup, "a") as tar:
            info = tarfile.TarInfo("data/harmony/config/devices.json")
            info.size = 2
            tar.addfile(info, io.BytesIO(b"{}"))

    def test_config_restore_on_an_emptied_hub_adds_no_stock_files(self):
        self.install_busybox("cache/bin/busybox")
        self.emptied_hub()
        self.config_backup()
        self.restore()  # no copy here: not needed
        self.assertTrue((self.root / "data" / "harmony" / "config" / "devices.json").is_file())
        self.stock_copy()
        self.install_busybox("cache/bin/busybox")  # the restore emptied /cache
        self.restore()  # a copy here: not put back unasked
        self.assertEqual(list((self.root / "data" / "resources").iterdir()), [])

    def test_stock_copy_named_on_the_command_line(self):
        self.install_busybox("cache/bin/busybox")
        self.emptied_hub()
        self.config_backup()
        self.restore(stock_copy=self.stock_copy("elsewhere.tar"))
        self.assertEqual((self.root / "data" / "resources" / "DeviceList.json").read_bytes(), self.STOCK_DATA)

    def test_unreadable_mac_asks_for_the_copy(self):
        self.install_busybox("cache/bin/busybox")
        self.emptied_hub()
        self.stock_copy()
        self.hub.mac = ""
        with self.assertRaises(RuntimeError) as ctx:
            self.restore()
        self.assertIn("--stock-resources", str(ctx.exception))
        self.assertFalse(any(c.startswith("killall") for c in self.hub.commands))

    def test_refuses_a_device_that_is_not_a_hub(self):
        """The remote answers at 192.168.2.1 with the same key: nothing may be stopped or emptied there."""
        self.install_busybox("cache/bin/busybox")
        self.hub.machine = "armv5tejl"
        with self.assertRaises(RuntimeError):
            self.restore()
        self.assertEqual(self.hub.commands, ["uname -m"])
        self.assertTrue((self.root / "cache" / "www_leftover").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
