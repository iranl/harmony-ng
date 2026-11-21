#!/usr/bin/env python3
"""Tests for tools/clean_stock_resources.py.

The shell commands run unchanged with sh in a temporary directory that stands for the Hub's
root (/data moved under it), so the scan, the delete and its checks are the real ones.
Only "uname -m" is answered here. No network.
"""

from __future__ import annotations

import contextlib
import io
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO_DIR / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import clean_stock_resources as csr  # noqa: E402
import ssh_hostkeys  # noqa: E402


class FakeHub:
    def __init__(self, root: pathlib.Path):
        self.root = root
        self.commands: list[str] = []
        self.fail_rm = False

    def run(self, cmd: str) -> subprocess.CompletedProcess:
        self.commands.append(cmd)
        if cmd == "uname -m":
            return subprocess.CompletedProcess(cmd, 0, "mips\n", "")
        root = self.root.as_posix()
        mapped = re.sub(r"(?<![\w./-])/data/", f"{root}/data/", cmd)
        if self.fail_rm:
            mapped = mapped.replace("rm -rf", "false")
        res = subprocess.run(["sh", "-c", mapped], capture_output=True, text=True)
        res.stdout = res.stdout.replace(root, "")
        return res


class TestCleanStockResources(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.res = self.root / "data" / "resources"
        (self.res / "icons").mkdir(parents=True)
        (self.root / "data" / "harmony" / "config").mkdir(parents=True)
        (self.res / "DeviceList.json").write_text("x" * 1000)
        (self.res / ".hidden").write_text("y" * 10)
        (self.res / "icons" / "tv icon.png").write_text("z" * 200)
        self.hub = FakeHub(self.root)
        env = mock.patch.dict(os.environ, {ssh_hostkeys.KNOWN_HOSTS_ENV: str(self.root / "known_hosts")})
        env.start()
        self.addCleanup(env.stop)

    def imported(self):
        (self.root / "data" / "harmony" / "config" / "devices.json").write_text('{"devices": []}')

    def saved(self):
        (self.root / "data" / "harmony" / "stock_resources_saved").write_text("stock_resources_abc.tar\n")

    def test_scan_walks_subdirectories_hidden_files_and_spaces(self):
        files, total = csr.scan_stock_resources(self.hub.run)
        self.assertEqual(sorted(files), [(".hidden", 10), ("DeviceList.json", 1000), ("icons/tv icon.png", 200)])
        self.assertEqual(total, 1210)
        # Stock BusyBox has no "ls -R" (and no find, du or wc)
        self.assertNotIn("ls -lAR", csr.SCAN_CMD)
        self.assertNotRegex(csr.SCAN_CMD, r"\b(find|du|wc)\b")

    def test_scan_refuses_a_link(self):
        other = self.root / "elsewhere"
        self.res.rename(other)
        self.res.symlink_to(other)
        with self.assertRaises(csr.LinkError):
            csr.scan_stock_resources(self.hub.run)

    def test_refusal_needs_the_import_and_a_saved_copy(self):
        self.assertIn("not imported", csr.refusal(self.hub.run))
        (self.root / "data" / "harmony" / "config" / "devices.json").write_text("")  # a cut upload
        self.assertIn("not imported", csr.refusal(self.hub.run))
        self.imported()
        self.assertIn("no saved copy", csr.refusal(self.hub.run))
        self.saved()
        self.assertIsNone(csr.refusal(self.hub.run))

    def test_empty_removes_everything_and_keeps_the_directory(self):
        self.assertIsNone(csr.empty_stock_resources(self.hub.run))
        self.assertTrue(self.res.is_dir())
        self.assertEqual(list(self.res.iterdir()), [])
        self.assertTrue((self.root / "data" / "harmony").is_dir())
        # "/data/resources/.*" also matches "..", that is /data
        self.assertNotIn("/.*", csr.EMPTY_CMD)

    def test_empty_reports_a_failed_rm(self):
        self.hub.fail_rm = True
        err = csr.empty_stock_resources(self.hub.run)
        self.assertIn("failed", err)
        self.assertTrue((self.res / "DeviceList.json").is_file())

    def test_empty_never_follows_a_link(self):
        other = self.root / "elsewhere"
        self.res.rename(other)
        self.res.symlink_to(other)
        self.assertIsNotNone(csr.empty_stock_resources(self.hub.run))
        self.assertTrue((other / "DeviceList.json").is_file())

    def test_ssh_uses_the_shared_host_key_options(self):
        with mock.patch("subprocess.run", return_value=subprocess.CompletedProcess([], 0, "", "")) as run:
            csr.ssh_runner("1.2.3.4", "root", [])("true")
        argv = run.call_args[0][0]
        self.assertIn("StrictHostKeyChecking=accept-new", argv)
        self.assertNotIn("StrictHostKeyChecking=no", argv)

    def run_main(self, *argv: str) -> int:
        with mock.patch.object(csr, "ssh_runner", return_value=self.hub.run), \
             mock.patch.object(csr.backup_hub, "backup_hub", return_value=False) as backup, \
             mock.patch("sys.argv", ["clean_stock_resources.py", "1.2.3.4", *argv]):
            rc = csr.main()
        self.backup_calls = backup.call_count
        return rc

    def test_cli_success_path(self):
        self.imported()
        self.saved()
        self.assertEqual(self.run_main("--yes"), 0)
        self.assertEqual(list(self.res.iterdir()), [])

    def test_cli_dry_run_changes_nothing(self):
        self.imported()
        self.saved()
        self.assertEqual(self.run_main("--dry-run"), 0)
        self.assertTrue((self.res / "DeviceList.json").is_file())

    def test_cli_without_saved_copy_runs_the_backup_and_refuses(self):
        self.imported()
        self.assertEqual(self.run_main("--yes"), 1)
        self.assertEqual(self.backup_calls, 1)
        self.assertTrue((self.res / "DeviceList.json").is_file())

    def test_cli_refuses_before_the_stock_import(self):
        self.saved()
        self.assertEqual(self.run_main("--yes"), 1)
        self.assertTrue((self.res / "DeviceList.json").is_file())

    def deploy(self, imported_now: bool = False):
        """build/deploy_hub.py --clean-stock-resources, run on the fake Hub"""
        sys.path.insert(0, str(REPO_DIR / "build"))
        import deploy_hub
        d = SimpleNamespace(run_ssh=self.hub.run, dry_run=False, deploy_failures=[], bundle_layout=False,
                            repo_root=REPO_DIR, script_dir=REPO_DIR / "build")
        d.dry_skip = lambda msg: False
        with contextlib.redirect_stdout(io.StringIO()):
            deploy_hub.HubDeployer.empty_stock_resources(d, imported_now)
        return d.deploy_failures

    def test_deploy_never_empties_in_the_run_that_imported(self):
        self.imported()
        self.saved()
        self.assertEqual(self.deploy(imported_now=True), [])
        self.assertTrue((self.res / "DeviceList.json").is_file())

    def test_deploy_refusal_and_failure_count_as_deploy_failures(self):
        self.imported()
        self.assertTrue(self.deploy())  # no saved copy
        self.assertTrue((self.res / "DeviceList.json").is_file())
        self.saved()
        self.hub.fail_rm = True
        self.assertTrue(self.deploy())
        self.hub.fail_rm = False
        self.assertEqual(self.deploy(), [])
        self.assertEqual(list(self.res.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
