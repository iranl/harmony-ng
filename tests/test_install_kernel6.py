#!/usr/bin/env python3
"""tools/install_kernel6.py picks the step from what the Hub runs, and tools/rollback_kernel6.py
builds the 10 MB data + cache image (or refuses a backup without one)."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import install_kernel6 as ik  # noqa: E402
import rollback_kernel6 as rk  # noqa: E402

OLD_MTD = 'mtd4: 00500000 00010000 "data"\nmtd5: 00500000 00010000 "cache"\n'
NEW_MTD = 'mtd4: 009f0000 00010000 "data"\nmtd5: 00010000 00010000 "state"\n'


class DetectMode(unittest.TestCase):
    def test_modes(self):
        self.assertEqual(ik.detect_mode("2.6.31-g89d565c\n", OLD_MTD), ik.STOCK)
        self.assertEqual(ik.detect_mode("6.18.65-harmony", OLD_MTD), ik.OLD_LAYOUT)
        self.assertEqual(ik.detect_mode("6.18.65-harmony", NEW_MTD), ik.CURRENT)
        with self.assertRaises(SystemExit):
            ik.detect_mode("3.3.8", NEW_MTD)


class DatacacheImage(unittest.TestCase):
    def test_backup_and_factory_fresh(self):
        with tempfile.TemporaryDirectory() as d:
            bk = pathlib.Path(d)
            with self.assertRaises(SystemExit):
                rk.datacache_image(bk, False)
            self.assertEqual(rk.datacache_image(bk, True), b"\xff" * 0xA00000)
            (bk / "data.bin").write_bytes(b"D" * 0x500000)
            (bk / "cache.bin").write_bytes(b"C" * 0x500000)
            img = rk.datacache_image(bk, False)
            self.assertEqual(img[:1] + img[-1:], b"DC")
            (bk / "cache.bin").write_bytes(b"C")
            with self.assertRaises(SystemExit):
                rk.datacache_image(bk, False)


if __name__ == "__main__":
    unittest.main()
