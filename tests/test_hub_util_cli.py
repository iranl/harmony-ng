#!/usr/bin/env python3
"""hub_util mknod and eth_fix report what they did (hub_util built for the host).

- mknod with a type other than c or b is refused; it used to create a regular file where the
  device node should be;
- eth_fix on an interface that does not exist exits 1 and does not print the success line; it
  printed "All ... offloads disabled successfully" whatever the ioctl calls returned.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SRC = REPO_DIR / "payload" / "source"


def hub_util_sources() -> list[str]:
    """hub_util's sources as build/compile_all.sh lists them (the list may grow)"""
    for line in (REPO_DIR / "build" / "compile_all.sh").read_text().splitlines():
        if '-o "$BIN/hub_util"' in line:
            return [str(REPO_DIR / t) for t in line.split() if t.startswith("payload/source/") and t.endswith(".c")]
    raise RuntimeError("hub_util build line not found in build/compile_all.sh")


class HubUtilCli(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.bin = pathlib.Path(cls.tmp.name) / "hub_util"
        srcs = hub_util_sources()
        subprocess.run(["gcc", "-Wall", "-O2", "-DTEST_HARNESS", f"-I{SRC}", "-o", str(cls.bin), *srcs, "-lm"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_mknod_bad_type_refused(self):
        node = pathlib.Path(self.tmp.name) / "node"
        res = subprocess.run([str(self.bin), "mknod", str(node), "x", "1", "2"], capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertFalse(node.exists(), "a regular file was created in place of a device node")

    def test_eth_fix_reports_failure(self):
        res = subprocess.run([str(self.bin), "eth_fix", "nosuchif0"], capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertNotIn("successfully", res.stdout)


if __name__ == "__main__":
    unittest.main()
