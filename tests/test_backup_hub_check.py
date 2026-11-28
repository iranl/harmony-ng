#!/usr/bin/env python3
"""tools/backup_hub.py must not save a cut tar stream as a good backup.

A fake ssh first in PATH (no network) answers the tool's probes and streams a real tar
archive for the backup commands, cut to a given length and ending with a given exit
code, as a dropped connection does. A cut or failed stream must leave no file under the
backup's name; a complete one is saved.

Commands on /data run with sh in a temporary directory that stands for the Hub's root: the
golden backup is made only while /data/resources holds the stock files.
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
  "uname -m") echo "${FAKE_MACHINE:-mips}" ;;
  *ath0/address*) echo "${FAKE_MAC-00:11:22:33:44:55}" ;;
  *"-cf - /data/resources")
    cd "$FAKE_ROOT" && tar -cf - ${FAKE_STOCK_PATHS:-data/resources} | head -c "${FAKE_STOCK_BYTES:-99999999}" ;;
  *"-cf -"*)
    head -c "$FAKE_TAR_BYTES" "$FAKE_TAR"
    exit "$FAKE_TAR_RC" ;;
  */data/*) sh -c "$(printf '%s' "$last" | sed "s#/data/#$FAKE_ROOT/data/#g")" | sed "s#$FAKE_ROOT##g" ;;
  *) exit 0 ;;
esac
"""

TAG = "001122334455"


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
        self.root = base / "hub"
        self.res = self.root / "data" / "resources"
        (self.res / "sub").mkdir(parents=True)
        (self.root / "data" / "harmony").mkdir()
        (self.res / "DeviceList.json").write_text('{"devices": []}')
        (self.res / "sub" / "Context.json").write_text("{}")
        self.log = base / "ssh.log"
        self.env = dict(os.environ)
        os.environ["PATH"] = f"{self.bin}:{os.environ['PATH']}"
        os.environ["FAKE_TAR"] = str(self.tar)
        os.environ["FAKE_ROOT"] = str(self.root)
        os.environ["FAKE_LOG"] = str(self.log)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.env)
        self.tmp.cleanup()

    def run_backup(self, nbytes: int | None = None, rc: int = 0) -> bool:
        os.environ["FAKE_TAR_BYTES"] = str(self.full if nbytes is None else nbytes)
        os.environ["FAKE_TAR_RC"] = str(rc)
        self.log.write_text("")
        with contextlib.redirect_stdout(io.StringIO()):
            return backup_hub.backup_hub("hub.invalid", ssh_key=None, backup_dir=self.backups,
                                         repo_root=pathlib.Path(self.tmp.name))

    def saved(self) -> list[str]:
        return sorted(p.name for p in self.backups.iterdir()) if self.backups.is_dir() else []

    def test_complete_backup_saved(self):
        self.assertTrue(self.run_backup())
        names = self.saved()
        self.assertIn(f"golden_hub_backup_{TAG}.tar", names)
        self.assertEqual(len([n for n in names if n.startswith(f"hub_backup_{TAG}_")]), 1, names)
        self.assertFalse([n for n in names if n.endswith(".part")])

    def test_stream_cut_inside_member(self):
        self.assertFalse(self.run_backup(3000, 255))
        self.assertEqual([n for n in self.saved() if "hub_backup" in n], [],
                         "a cut stream must not be saved as golden or rolling backup")

    def test_stream_cut_on_block_boundary(self):
        # tarfile alone reads this as a valid, shorter archive
        self.assertFalse(self.run_backup(512 * 14, 0))
        self.assertEqual([n for n in self.saved() if "hub_backup" in n], [])

    def test_no_copy_of_the_stock_files(self):
        # the stock resources delete is gone, and with it the copy it needed
        self.assertTrue(self.run_backup())
        self.assertFalse([n for n in self.saved() if n.startswith("stock_resources_")])
        self.assertFalse((self.root / "data" / "harmony" / "stock_resources_saved").exists())

    def test_no_golden_once_the_stock_files_are_gone(self):
        for f in (self.res / "DeviceList.json", self.res / "sub" / "Context.json"):
            f.unlink()
        self.assertTrue(self.run_backup())
        names = self.saved()
        self.assertNotIn(f"golden_hub_backup_{TAG}.tar", names)
        self.assertEqual(len([n for n in names if n.startswith(f"hub_backup_{TAG}_")]), 1, names)

    def test_ssh_failure_with_full_stream(self):
        self.assertFalse(self.run_backup(self.full, 255))
        self.assertEqual([n for n in self.saved() if "hub_backup" in n], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
