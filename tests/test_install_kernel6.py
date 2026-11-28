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


class FakeHub:
    """Answers like a freshly rooted stock Hub: no df anywhere, the static hub_util has one"""
    def __init__(self):
        self.cmds, self.pushed = [], []

    def run(self, remote, check=True, timeout=600):
        self.cmds.append(remote)
        if "k6_hub_util df" in remote:
            return "4321\n"
        return ""

    def push(self, local, remote):
        self.pushed.append(remote)


class StockHubWithoutHarmonyNG(unittest.TestCase):
    def test_free_space_from_the_static_hub_util(self):
        hub = FakeHub()
        inst = ik.Installer(hub, pathlib.Path("files"), pathlib.Path("scripts"), None, pathlib.Path("bk"))
        self.assertEqual(inst.free_kb("/data"), 4321, "no df on the Hub: hub_util df answers")
        self.assertEqual(hub.pushed, ["/tmp/k6_hub_util"])

    def test_ssh_server_kept_until_the_kernel_is_written(self):
        self.assertFalse(any("dropbear" in p for p in ik.STOCK_ERA), ik.STOCK_ERA)
        self.assertNotIn("/data/harmony/bin", ik.STOCK_ERA)
        # the loop itself, on a copy of the folder
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            b = pathlib.Path(d) / "bin"
            b.mkdir()
            for n in ("dropbearmulti", "dropbear", "scp", "ssh", "hub_daemon", "hub_rf", "busybox"):
                (b / n).write_text("x")
            subprocess.run(["sh", "-c", ik.STOCK_ERA_BIN.replace("/data/harmony/bin", str(b))], check=True)
            self.assertEqual(sorted(p.name for p in b.iterdir()), ["dropbear", "dropbearmulti", "scp", "ssh"])

    def test_refused_before_anything_is_deleted(self):
        # free space plus every file a deploy puts back is not enough: nothing removed, nothing stopped
        class Tight(FakeHub):
            def run(self, remote, check=True, timeout=600):
                self.cmds.append(remote)
                if "k6_hub_util df" in remote:
                    return "100\n"
                if remote.startswith("ls -lR"):
                    return "50\n"
                return ""
        hub = Tight()
        with tempfile.TemporaryDirectory() as d:
            files = pathlib.Path(d)
            (files / "harmony-hub-rootfs.sqfs").write_bytes(b"R" * 3_000_000)
            inst = ik.Installer(hub, files, files, None, files / "bk")
            inst.rescue_bb = None
            with self.assertRaises(SystemExit) as ctx:
                inst.from_stock_room()
        self.assertIn("Nothing was changed", str(ctx.exception))
        self.assertFalse([c for c in hub.cmds if c.startswith("rm ") or "killall" in c or "for f in" in c], hub.cmds)


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
