#!/usr/bin/env python3
"""tools/backup_hub.py must not save a cut tar stream as a good backup.

A fake ssh first in PATH (no network) answers the tool's probes and streams a real tar
archive for the backup commands, cut to a given length and ending with a given exit
code, as a dropped connection does. A cut or failed stream must leave no file under the
backup's name; a complete one is saved.

The stock files part runs for real: commands on /data run with sh in a temporary directory
that stands for the Hub's root, and the copy of /data/resources is a real tar of it.
"""

from __future__ import annotations

import contextlib
import io
import os
import pathlib
import sys
import tarfile
import tempfile
import time
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
STOCK_COPY = f"stock_resources_{TAG}.tar"


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
        self.mark = self.root / "data" / "harmony" / "stock_resources_saved"
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

    def stock_streamed(self) -> bool:
        return any(line.endswith("-cf - /data/resources") for line in self.log.read_text().splitlines())

    def hub_files(self) -> backup_hub.StockFiles:
        return backup_hub.archive_stock_files(self.backups / STOCK_COPY) or {}

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

    def test_stock_files_saved_once_and_hub_marked(self):
        self.assertTrue(self.run_backup())
        files = self.hub_files()
        self.assertEqual(sorted(files), ["DeviceList.json", "sub/Context.json"])
        self.assertEqual(self.mark.read_text().strip(), f"{STOCK_COPY} {backup_hub.stock_fingerprint(files)}")
        # Kept for good: a second run checks it against the Hub and does not stream it again
        mtime = (self.backups / STOCK_COPY).stat().st_mtime_ns
        self.assertTrue(self.run_backup())
        self.assertEqual((self.backups / STOCK_COPY).stat().st_mtime_ns, mtime)
        self.assertFalse(self.stock_streamed())

    def test_changed_files_get_a_new_copy_beside_the_old(self):
        self.assertTrue(self.run_backup())
        old = (self.backups / STOCK_COPY).read_bytes()
        (self.res / "DeviceList.json").write_text('{"devices": [1]}')
        time.sleep(1.1)  # the new copy's name carries the time
        self.assertTrue(self.run_backup())
        self.assertEqual((self.backups / STOCK_COPY).read_bytes(), old)
        newer = [n for n in self.saved() if n.startswith(f"stock_resources_{TAG}_")]
        self.assertEqual(len(newer), 1, self.saved())
        self.assertTrue(self.mark.read_text().startswith(newer[0] + " "))

    def test_a_copy_missing_files_is_not_kept(self):
        os.environ["FAKE_STOCK_PATHS"] = "data/resources/DeviceList.json"
        self.run_backup()
        self.assertNotIn(STOCK_COPY, self.saved())
        self.assertFalse(self.mark.exists())

    def test_a_cut_copy_is_not_kept(self):
        os.environ["FAKE_STOCK_BYTES"] = "1024"
        self.run_backup()
        self.assertFalse([n for n in self.saved() if n.startswith("stock_resources_")])
        self.assertFalse(self.mark.exists())

    def test_no_copy_or_mark_without_the_mac(self):
        os.environ["FAKE_MAC"] = ""
        self.run_backup()
        self.assertFalse([n for n in self.saved() if n.startswith("stock_resources_")])
        self.assertFalse(self.mark.exists())

    def test_no_golden_once_the_stock_files_are_gone(self):
        for f in (self.res / "DeviceList.json", self.res / "sub" / "Context.json"):
            f.unlink()
        self.assertTrue(self.run_backup())
        names = self.saved()
        self.assertNotIn(f"golden_hub_backup_{TAG}.tar", names)
        self.assertFalse([n for n in names if n.startswith("stock_resources_")])
        self.assertEqual(len([n for n in names if n.startswith(f"hub_backup_{TAG}_")]), 1, names)

    def test_ssh_failure_with_full_stream(self):
        self.assertFalse(self.run_backup(self.full, 255))
        self.assertEqual([n for n in self.saved() if "hub_backup" in n], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
