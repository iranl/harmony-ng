#!/usr/bin/env python3
"""payload/scripts/update.sh apply_kernel6: the Linux 6.18 system image of a release, on a Hub that
runs it. The function runs alone in a harness: /proc and /cache move into a temporary root,
uname, df, the download, the signature check and hub_util are stubs.

The root file system must be staged in /cache before the kernel is written (a failure leaves the
old kernel with the old root file system), nothing may be written when a file does not match the
signed manifest or /cache is too small, and a kernel write that fails removes the staged image.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "update.sh"


def function_text(name: str) -> str:
    text = SCRIPT.read_text()
    m = re.search(rf"^{name}\(\) \{{\n.*?^\}}\n", text, re.S | re.M)
    assert m, name
    return m.group(0)


class ApplyKernel6(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        r = self.root = pathlib.Path(self.tmp.name)
        for d in ("proc", "cache/kernel6", "tmpdir", "serve", "stub", "tmp"):
            (r / d).mkdir(parents=True)
        (r / "proc/mtd").write_text(
            'dev:    size   erasesize  name\nmtd1: 000f0000 00010000 "kernel"\n'
            'mtd2: 000f0000 00010000 "kernel2"\nmtd3: 00400000 00010000 "root"\n')
        (r / "proc/mounts").write_text("overlay / overlay rw 0 0\n")
        (r / "tmpdir/manifest.json").write_text(
            '{"files": {"kernel6/harmony-hub-flash.uImage": {}, "kernel6/harmony-hub-rootfs.sqfs": {}}}')
        (r / "serve/harmony-hub-flash.uImage").write_bytes(b"K" * 1000)
        (r / "serve/harmony-hub-rootfs.sqfs").write_bytes(b"R" * 2048)
        (r / "cache/kernel6/modules").mkdir()
        self.log = r / "calls"
        stubs = {
            "uname": "echo 6.18.65-harmony",
            "df": 'echo "Filesystem 1K-blocks Used Available"; echo "mtd:cache 5120 0 ${CACHE_FREE:-4000}"',
            "killall": "exit 0",
            "sleep": "exit 0",
        }
        for name, body in stubs.items():
            f = r / "stub" / name
            f.write_text(f"#!/bin/sh\n{body}\n")
            f.chmod(0o755)
        hub_util = r / "stub" / "hub_util"
        # FLASH_RC: every write's exit code; FLASH_FAIL_ONCE=1: only the first write fails.
        # Each call records whether the services' stop flag was there.
        hub_util.write_text(
            f"#!/bin/sh\nstop=no; [ -e '{r}/tmp/harmony_stop' ] && stop=yes\n"
            f"echo \"hub_util $* stop=$stop\" >> '{self.log}'\n"
            f"if [ \"${{FLASH_FAIL_ONCE:-}}\" = 1 ] && [ ! -e '{r}/failed_once' ]; then : > '{r}/failed_once'; exit 1; fi\n"
            "exit ${FLASH_RC:-0}\n")
        hub_util.chmod(0o755)
        func = function_text("apply_kernel6")
        func = re.sub(r"(?<![\w/.-])/(proc|cache|tmp)/", lambda m: f"{r}{m.group(0)}", func)
        harness = f"""
TMPDIR='{r}/tmpdir'
REPO=http://pkg.invalid
TIMESTAMP=1
HUB_UTIL='{r}/stub/hub_util'
download_retry() {{ cp "{r}/serve/$(basename "${{1%%\\?*}}")" "$2"; }}
verify_files() {{ [ "${{BAD_SIG:-}}" != 1 ]; }}
{func}
apply_kernel6
echo "rc=$?"
"""
        self.harness = r / "harness.sh"
        self.harness.write_text(harness)

    def tearDown(self):
        self.tmp.cleanup()

    def run_it(self, **env) -> str:
        import os
        e = dict(os.environ, PATH=f"{self.root}/stub:/usr/bin:/bin", **env)
        return subprocess.run(["sh", str(self.harness)], env=e, capture_output=True, text=True, timeout=60).stdout

    def calls(self) -> list[str]:
        return self.log.read_text().splitlines() if self.log.exists() else []

    def staged(self) -> pathlib.Path:
        return self.root / "cache/kernel6/rootfs.sqfs"

    def test_stages_root_then_writes_kernel(self):
        out = self.run_it()
        self.assertIn("rc=0", out)
        self.assertEqual(self.staged().read_bytes(), b"R" * 2048)
        self.assertEqual(len(self.calls()), 1)
        self.assertRegex(self.calls()[0], r"flash-image \S+harmony-hub-flash.uImage /dev/mtd1 /dev/mtd2 stop=yes$")
        self.assertFalse((self.root / "cache/kernel6/modules").exists())  # root file system boot

    def test_bad_signature_changes_nothing(self):
        out = self.run_it(BAD_SIG="1")
        self.assertIn("rc=1", out)
        self.assertFalse(self.staged().exists())
        self.assertEqual(self.calls(), [])

    def test_no_room_in_cache_writes_no_kernel(self):
        out = self.run_it(CACHE_FREE="1")
        self.assertIn("rc=1", out)
        self.assertFalse(self.staged().exists())
        self.assertEqual(self.calls(), [])

    def test_failed_kernel_write_removes_staged_root(self):
        # two failed writes: the update fails (2) with a one-line error, no new root stays staged
        out = self.run_it(FLASH_RC="1")
        self.assertIn("rc=2", out)
        self.assertFalse(self.staged().exists())
        self.assertEqual(len(self.calls()), 2)
        err = [l for l in out.splitlines() if l.startswith("[-]")]
        self.assertEqual(len(err), 1)
        self.assertIn("kernel partition is incomplete", err[0])

    def test_kernel_write_retried_with_the_services_stopped(self):
        out = self.run_it(FLASH_FAIL_ONCE="1")
        self.assertIn("rc=0", out)
        self.assertEqual(len(self.calls()), 2)
        self.assertTrue(all(c.endswith("stop=yes") for c in self.calls()), self.calls())
        self.assertTrue(self.staged().exists(), "the retry worked: the root stays staged")

    def test_old_flash_layout_is_left_to_the_install_tool(self):
        mtd = self.root / "proc/mtd"
        mtd.write_text(mtd.read_text() + 'mtd4: 00500000 00010000 "data"\nmtd5: 00500000 00010000 "cache"\n')
        out = self.run_it()
        self.assertIn("rc=1", out)
        self.assertIn("install_kernel6.py", out)
        self.assertFalse(self.staged().exists())
        self.assertEqual(self.calls(), [])

    def test_release_without_system_image(self):
        (self.root / "tmpdir/manifest.json").write_text('{"files": {}}')
        out = self.run_it()
        self.assertIn("rc=1", out)
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
