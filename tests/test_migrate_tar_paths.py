#!/usr/bin/env python3
"""tools/migrate_hub_configs.py must not write tar.gz members outside its extraction folder.

Python 3.14 filters extracted members by default; 3.11 to 3.13 do not. The test sets the
tarfile default to the unfiltered behaviour of those versions, then migrates archives
whose members are named ../x or a/../../x: nothing may land outside the temp folder.
A normal Hub archive (data/harmony/config/...) must still migrate.
"""

from __future__ import annotations

import io
import json
import pathlib
import sys
import tarfile
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import migrate_hub_configs as mig  # noqa: E402


def add(tf: tarfile.TarFile, name: str, data: bytes) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    tf.addfile(info, io.BytesIO(data))


class MigrateTarPaths(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        # Work below base/a/b/c so "../" names stay inside the test's own folder
        self.work = self.base / "a" / "b" / "c"
        self.work.mkdir(parents=True)
        self.saved_filter = tarfile.TarFile.extraction_filter
        tarfile.TarFile.extraction_filter = staticmethod(tarfile.fully_trusted_filter)
        self.saved_tempdir = tempfile.tempdir
        tempfile.tempdir = str(self.work)

    def tearDown(self):
        tarfile.TarFile.extraction_filter = self.saved_filter
        tempfile.tempdir = self.saved_tempdir
        self.tmp.cleanup()

    def archive(self, members: dict[str, bytes]) -> pathlib.Path:
        path = self.base / "backup.tar.gz"
        with tarfile.open(path, "w:gz", format=tarfile.GNU_FORMAT) as tf:
            for name, data in members.items():
                add(tf, name, data)
        return path

    def escaped(self) -> list[str]:
        return sorted(str(p.relative_to(self.base)) for p in self.base.rglob("ESCAPED*"))

    def test_dotdot_member_refused(self):
        src = self.archive({"../ESCAPED_dotdot.txt": b"x", "../../ESCAPED_dotdot2.txt": b"x"})
        with self.assertRaises(Exception):
            mig.migrate(str(src), str(self.base / "out"))
        self.assertEqual(self.escaped(), [], "a member named ../x was written outside the extraction folder")

    def test_nested_dotdot_member_refused(self):
        src = self.archive({"data/harmony/../../../ESCAPED_nested.txt": b"x"})
        with self.assertRaises(Exception):
            mig.migrate(str(src), str(self.base / "out"))
        self.assertEqual(self.escaped(), [])

    def test_hub_archive_still_migrates(self):
        devices = {"devices": [{"id": "1", "name": "TV", "manufacturer": "M", "model": "X", "type": "IR",
                                "commands": [{"name": "PowerOn", "keyCode": "G:NEC:()(0x20DF10EF)():3"}]}]}
        src = self.archive({
            "data/harmony/config/devices.json": json.dumps(devices).encode(),
            "data/harmony/config/activities.json": json.dumps({"activities": []}).encode(),
        })
        stats = mig.migrate(str(src), str(self.base / "out"))
        self.assertEqual(stats["devices"], 1, stats)
        self.assertEqual(self.escaped(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
