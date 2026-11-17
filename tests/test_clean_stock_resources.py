#!/usr/bin/env python3
"""Tests for tools/clean_stock_resources.py."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO_DIR / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import clean_stock_resources  # noqa: E402


class TestCleanStockResources(unittest.TestCase):
    def test_scan_stock_resources_busybox_format(self):
        sample_output = """
-rw-r--r--    1 0        0            6753 CapabilityList.json
-rw-r--r--    1 0        0              39 MapList.json
-rw-r--r--    1 0        0          133629 DeviceList.json
-rw-r--r--    1 0        0           79434 FunctionList.json
-rw-r--r--    1 0        0             514 AutomationConfig.json
-rw-r--r--    1 0        0            1343 index.json
-rw-r--r--    1 0        0           21383 ActivityList.json
-rw-r--r--    1 0        0             948 Context.json
-rw-r--r--    1 0        0            9335 ProtocolList.json
-rw-r--r--    1 0        0            1019 Settings.json
"""
        with mock.patch("clean_stock_resources.run_ssh_cmd") as mock_run:
            mock_run.return_value = subprocess.CompletedProcess(
                args=["ssh"], returncode=0, stdout=sample_output, stderr=""
            )
            files, total_bytes = clean_stock_resources.scan_stock_resources("1.2.3.4", "root", [])
            self.assertEqual(len(files), 10)
            self.assertEqual(total_bytes, 254397)
            fnames = [f[0] for f in files]
            self.assertIn("DeviceList.json", fnames)
            self.assertIn("ActivityList.json", fnames)
            self.assertIn("FunctionList.json", fnames)

    def test_scan_stock_resources_empty(self):
        with mock.patch("clean_stock_resources.run_ssh_cmd") as mock_run:
            mock_run.return_value = subprocess.CompletedProcess(
                args=["ssh"], returncode=0, stdout="", stderr=""
            )
            files, total_bytes = clean_stock_resources.scan_stock_resources("1.2.3.4", "root", [])
            self.assertEqual(files, [])
            self.assertEqual(total_bytes, 0)

    def test_empty_stock_resources_calls_sync(self):
        with mock.patch("clean_stock_resources.run_ssh_cmd") as mock_run:
            mock_run.return_value = subprocess.CompletedProcess(
                args=["ssh"], returncode=0, stdout="", stderr=""
            )
            ok = clean_stock_resources.empty_stock_resources("1.2.3.4", "root", [])
            self.assertTrue(ok)
            self.assertTrue(mock_run.called)
            cmd = mock_run.call_args[0][2]
            self.assertIn("rm -rf /data/resources/*", cmd)
            self.assertIn("hub_sync", cmd)

    def test_cli_dry_run_exits_cleanly(self):
        with mock.patch("clean_stock_resources.scan_stock_resources") as mock_scan:
            mock_scan.return_value = ([("DeviceList.json", 100000)], 100000)
            with mock.patch("clean_stock_resources.empty_stock_resources") as mock_empty:
                with mock.patch("sys.argv", ["clean_stock_resources.py", "1.2.3.4", "--dry-run"]):
                    rc = clean_stock_resources.main()
                    self.assertEqual(rc, 0)
                    self.assertFalse(mock_empty.called)


if __name__ == "__main__":
    unittest.main()
