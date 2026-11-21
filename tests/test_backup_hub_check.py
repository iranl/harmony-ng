#!/usr/bin/env python3
"""tools/backup_hub.py must not save a cut tar stream as a good backup.

A fake ssh first in PATH (no network) answers the tool's probes and streams a real tar
archive for the backup commands, cut to a given length and ending with a given exit
code, as a dropped connection does. A cut or failed stream must leave no file under the
backup's name; a complete one is saved.
"""

from __future__ import annotations

import contextlib
import io
import os
import pathlib
import sys
import tarfile
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR))

from tools import backup_hub  # noqa: E402

FAKE_SSH = """#!/bin/sh
for last; do :; done
[ -n "$FAKE_LOG" ] && echo "$last" >> "$FAKE_LOG"
case "$last" in
  *"stock_resources_saved ] && [ ! -f"*) [ -n "$FAKE_EMPTIED" ] && echo "$FAKE_EMPTIED"; exit 0 ;;
  *"-cf - /data/resources 2>"*) cat "$FAKE_STOCK_TAR"; exit 0 ;;
  *"DeviceList.json ] && echo 1"*) if [ -n "$FAKE_STOCK_TAR" ]; then echo 1; else echo 0; fi ;;
  *"-cf -"*)
    head -c "$FAKE_TAR_BYTES" "$FAKE_TAR"
    exit "$FAKE_TAR_RC" ;;
  "uname -m") echo "${FAKE_MACHINE:-mips}" ;;
  *ath0/address*) echo "${FAKE_MAC:-00:11:22:33:44:55}" ;;
  *) exit 0 ;;
esac
"""


def make_tar(path: pathlib.Path) -> int:
    with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as tf:
        for i in range(20):
            data = os.urandom(3000)
            info = tarfile.TarInfo(f"data/harmony/file{i}.json")
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    return path.stat().st_size


class BackupHubCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.bin = base / "bin"
        self.bin.mkdir()
        ssh = self.bin / "ssh"
        ssh.write_text(FAKE_SSH)
        ssh.chmod(0o755)
        self.tar = base / "hub.tar"
        self.full = make_tar(self.tar)
        self.backups = base / "backups"
        self.env = dict(os.environ)
        os.environ["PATH"] = f"{self.bin}:{os.environ['PATH']}"
        os.environ["FAKE_TAR"] = str(self.tar)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.env)
        self.tmp.cleanup()

    def run_backup(self, nbytes: int, rc: int) -> bool:
        os.environ["FAKE_TAR_BYTES"] = str(nbytes)
        os.environ["FAKE_TAR_RC"] = str(rc)
        with contextlib.redirect_stdout(io.StringIO()):
            return backup_hub.backup_hub("hub.invalid", ssh_key=None, backup_dir=self.backups,
                                         repo_root=pathlib.Path(self.tmp.name))

    def saved(self) -> list[str]:
        return sorted(p.name for p in self.backups.iterdir()) if self.backups.is_dir() else []

    def test_complete_backup_saved(self):
        self.assertTrue(self.run_backup(self.full, 0))
        names = self.saved()
        self.assertIn("golden_hub_backup_001122334455.tar", names)
        self.assertEqual(len([n for n in names if n.startswith("hub_backup_001122334455_")]), 1, names)
        self.assertFalse([n for n in names if n.endswith(".part")])

    def test_stream_cut_inside_member(self):
        self.assertFalse(self.run_backup(3000, 255))
        self.assertEqual(self.saved(), [], "a cut stream must not be saved as golden or rolling backup")

    def test_stream_cut_on_block_boundary(self):
        # tarfile alone reads this as a valid, shorter archive
        self.assertFalse(self.run_backup(512 * 14, 0))
        self.assertEqual(self.saved(), [])

    def test_stock_files_saved_once_and_hub_marked(self):
        stock = pathlib.Path(self.tmp.name) / "stock.tar"
        with tarfile.open(stock, "w", format=tarfile.USTAR_FORMAT) as tf:
            info = tarfile.TarInfo("data/resources/DeviceList.json")
            info.size = 3
            tf.addfile(info, io.BytesIO(b"{}\n"))
        log = pathlib.Path(self.tmp.name) / "ssh.log"
        os.environ["FAKE_STOCK_TAR"] = str(stock)
        os.environ["FAKE_LOG"] = str(log)
        self.assertTrue(self.run_backup(self.full, 0))
        saved = self.backups / "stock_resources_001122334455.tar"
        self.assertTrue(backup_hub.archive_has_stock_resources(saved))
        self.assertIn("echo stock_resources_001122334455.tar > /data/harmony/stock_resources_saved", log.read_text())
        # Kept for good: a second run does not stream it again
        mtime = saved.stat().st_mtime_ns
        log.write_text("")
        self.assertTrue(self.run_backup(self.full, 0))
        self.assertEqual(saved.stat().st_mtime_ns, mtime)
        self.assertNotIn("-cf - /data/resources 2>", log.read_text())

    def test_stock_copy_without_device_list_is_not_kept(self):
        stock = pathlib.Path(self.tmp.name) / "stock.tar"
        make_tar(stock)  # no data/resources/DeviceList.json in it
        log = pathlib.Path(self.tmp.name) / "ssh.log"
        os.environ["FAKE_STOCK_TAR"] = str(stock)
        os.environ["FAKE_LOG"] = str(log)
        self.run_backup(self.full, 0)
        self.assertNotIn("stock_resources_001122334455.tar", self.saved())
        self.assertNotIn("> /data/harmony/stock_resources_saved", log.read_text())

    def test_no_golden_once_the_stock_files_are_gone(self):
        os.environ["FAKE_EMPTIED"] = "stock_resources_001122334455.tar"
        self.assertTrue(self.run_backup(self.full, 0))
        names = self.saved()
        self.assertNotIn("golden_hub_backup_001122334455.tar", names)
        self.assertEqual(len([n for n in names if n.startswith("hub_backup_001122334455_")]), 1, names)

    def test_ssh_failure_with_full_stream(self):
        self.assertFalse(self.run_backup(self.full, 255))
        self.assertEqual(self.saved(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
