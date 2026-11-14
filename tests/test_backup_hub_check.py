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
case "$last" in
  *"-cf -"*)
    head -c "$FAKE_TAR_BYTES" "$FAKE_TAR"
    exit "$FAKE_TAR_RC" ;;
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
        self.assertIn("golden_hub_backup.tar", names)
        self.assertEqual(len([n for n in names if n.startswith("hub_backup_")]), 1, names)
        self.assertFalse([n for n in names if n.endswith(".part")])

    def test_stream_cut_inside_member(self):
        self.assertFalse(self.run_backup(3000, 255))
        self.assertEqual(self.saved(), [], "a cut stream must not be saved as golden or rolling backup")

    def test_stream_cut_on_block_boundary(self):
        # tarfile alone reads this as a valid, shorter archive
        self.assertFalse(self.run_backup(512 * 14, 0))
        self.assertEqual(self.saved(), [])

    def test_ssh_failure_with_full_stream(self):
        self.assertFalse(self.run_backup(self.full, 255))
        self.assertEqual(self.saved(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
