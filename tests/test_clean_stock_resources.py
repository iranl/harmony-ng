#!/usr/bin/env python3
"""Tests for tools/clean_stock_resources.py."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO_DIR / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import clean_stock_resources  # noqa: E402
import ssh_hostkeys  # noqa: E402


def done(stdout: str = "", rc: int = 0) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(args=["ssh"], returncode=rc, stdout=stdout, stderr="")


class TestCleanStockResources(unittest.TestCase):
    def setUp(self):
        # Keep the host key file of the test away from the user's own
        self.tmp = tempfile.TemporaryDirectory()
        env = mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: os.path.join(self.tmp.name, "known_hosts")})
        env.start()
        self.addCleanup(env.stop)
        self.addCleanup(self.tmp.cleanup)

    def test_scan_stock_resources_busybox_format(self):
        sample_output = """/data/resources:
-rw-r--r--    1 0        0            6753 Jan  1  2000 CapabilityList.json
-rw-r--r--    1 0        0              39 Jan  1  2000 MapList.json
-rw-r--r--    1 0        0          133629 Jan  1  2000 DeviceList.json
-rw-r--r--    1 0        0           79434 Jan  1  2000 FunctionList.json
-rw-r--r--    1 0        0             514 Jan  1  2000 AutomationConfig.json
-rw-r--r--    1 0        0            1343 Jan  1  2000 index.json
-rw-r--r--    1 0        0           21383 Jan  1  2000 ActivityList.json
-rw-r--r--    1 0        0             948 Jan  1  2000 Context.json
-rw-r--r--    1 0        0            9335 Jan  1  2000 ProtocolList.json
-rw-r--r--    1 0        0            1019 Jan  1  2000 Settings.json
"""
        with mock.patch("clean_stock_resources.run_ssh_cmd", return_value=done(sample_output)):
            files, total_bytes = clean_stock_resources.scan_stock_resources("1.2.3.4", "root", [])
        self.assertEqual(len(files), 10)
        self.assertEqual(total_bytes, 254397)
        fnames = [f[0] for f in files]
        self.assertIn("DeviceList.json", fnames)
        self.assertIn("ActivityList.json", fnames)
        self.assertIn("FunctionList.json", fnames)

    def test_scan_counts_files_in_subdirectories(self):
        sample_output = """/data/resources:
drwxr-xr-x    2 0        0               0 Jan  1  2000 icons
-rw-r--r--    1 0        0             100 Jan  1  2000 Context.json

/data/resources/icons:
-rw-r--r--    1 0        0             200 Jan  1  2000 tv icon.png
"""
        with mock.patch("clean_stock_resources.run_ssh_cmd", return_value=done(sample_output)):
            files, total_bytes = clean_stock_resources.scan_stock_resources("1.2.3.4", "root", [])
        self.assertEqual(files, [("Context.json", 100), ("icons/tv icon.png", 200)])
        self.assertEqual(total_bytes, 300)

    def test_scan_stock_resources_empty(self):
        with mock.patch("clean_stock_resources.run_ssh_cmd", return_value=done("/data/resources:\n")):
            files, total_bytes = clean_stock_resources.scan_stock_resources("1.2.3.4", "root", [])
        self.assertEqual(files, [])
        self.assertEqual(total_bytes, 0)

    def test_empty_never_globs_dot_entries(self):
        with mock.patch("clean_stock_resources.run_ssh_cmd", return_value=done()) as mock_run:
            ok = clean_stock_resources.empty_stock_resources("1.2.3.4", "root", [])
        self.assertTrue(ok)
        cmd = mock_run.call_args[0][2]
        # "/data/resources/.*" matches "..", which is /data
        self.assertNotIn("/.*", cmd)
        self.assertIn("rm -rf /data/resources;", cmd)
        self.assertIn("hub_sync", cmd)

    def test_ssh_uses_the_shared_host_key_options(self):
        with mock.patch("subprocess.run", return_value=done()) as mock_run:
            clean_stock_resources.run_ssh_cmd("1.2.3.4", "root", "true", [])
        argv = mock_run.call_args[0][0]
        self.assertIn("StrictHostKeyChecking=accept-new", argv)
        self.assertNotIn("StrictHostKeyChecking=no", argv)

    def run_main(self, *argv: str, migrated: bool = True):
        with mock.patch("clean_stock_resources.scan_stock_resources",
                        return_value=([("DeviceList.json", 100000)], 100000)), \
             mock.patch("clean_stock_resources.has_migrated_config", return_value=migrated), \
             mock.patch("clean_stock_resources.empty_stock_resources") as mock_empty, \
             mock.patch("sys.argv", ["clean_stock_resources.py", "1.2.3.4", *argv]):
            rc = clean_stock_resources.main()
        return rc, mock_empty

    def test_cli_dry_run_exits_cleanly(self):
        rc, mock_empty = self.run_main("--dry-run")
        self.assertEqual(rc, 0)
        self.assertFalse(mock_empty.called)

    def test_cli_refuses_before_the_stock_import(self):
        rc, mock_empty = self.run_main("--yes", migrated=False)
        self.assertEqual(rc, 1)
        self.assertFalse(mock_empty.called)


if __name__ == "__main__":
    unittest.main()
