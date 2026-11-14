#!/usr/bin/env python3
"""Free space check of build/deploy_hub.py before an upload.

The check runs on the Hub as a shell snippet around `df -k`. Here it runs in a
local sh against a stub `df` that prints what the Hub prints, so the awk part
is exercised too. No network: ssh and scp are replaced.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))

import deploy_hub  # noqa: E402

REAL_RUN = subprocess.run

DF_HEADER = "Filesystem           1K-blocks      Used Available Use% Mounted on\n"
# Stock Hub, kernel 2.6.31: /tmp is a tmpfs mounted without size=, df prints zeros.
DF_TMPFS_NO_SIZE = DF_HEADER + "tmpfs                        0         0         0   0% /var/volatile/tmp\n"
# Flash partition that really is almost full.
DF_FLASH_FULL = DF_HEADER + "/dev/mtdblock5            7680      7670        10 100% /data\n"
# BusyBox puts a long device name on its own line.
DF_WRAPPED = DF_HEADER + "/dev/very/long/device/name/of/the/partition\n" + "                          7680      2048      5632  27% /data\n"


class SpaceCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self.stub_dir = self.root / "stub"
        self.stub_dir.mkdir()
        self.src = self.root / "remote_ui.ota.gz"
        self.src.write_bytes(os.urandom(490 * 1024))
        # Destination lives inside the temp dir: the snippet's mkdir/rm stay local.
        self.dst = str(self.root / "hub_tmp" / "remote_ui.ota.gz")
        self.deployer = deploy_hub.HubDeployer(hub_host="hub.invalid", ssh_key="/nonexistent")
        self.ssh_calls: list[str] = []
        self.deployer.run_ssh = self.fake_ssh

    def tearDown(self):
        self.tmp.cleanup()

    def set_df(self, output: str) -> None:
        stub = self.stub_dir / "df"
        stub.write_text("#!/bin/sh\ncat <<'EOF'\n" + output + "EOF\n")
        stub.chmod(0o755)

    def fake_ssh(self, cmd: str) -> subprocess.CompletedProcess[str]:
        self.ssh_calls.append(cmd)
        if "df -k" in cmd:
            env = dict(os.environ, PATH=f"{self.stub_dir}:{os.environ.get('PATH', '')}")
            return REAL_RUN(["sh", "-c", cmd], capture_output=True, text=True, env=env)
        if "md5sum" in cmd:
            return subprocess.CompletedProcess(cmd, 0, "ok\n", "")
        return subprocess.CompletedProcess(cmd, 0, "", "")

    def deploy(self) -> bool:
        def fake_run(cmd, *args, **kwargs):
            if cmd and cmd[0] == "scp":
                return subprocess.CompletedProcess(cmd, 0, "", "")
            return REAL_RUN(cmd, *args, **kwargs)

        with patch.object(deploy_hub.subprocess, "run", side_effect=fake_run):
            return self.deployer.deploy_file_if_changed(self.src, self.dst, "644")

    def test_tmpfs_without_size_is_not_full(self):
        self.set_df(DF_TMPFS_NO_SIZE)
        self.assertTrue(self.deploy(), self.deployer.deploy_failures)
        self.assertEqual(self.deployer.deploy_failures, [])

    def test_full_flash_is_still_refused(self):
        self.set_df(DF_FLASH_FULL)
        self.assertFalse(self.deploy())
        self.assertEqual(self.deployer.deploy_failures, [f"{self.dst} (no space)"])

    def test_wrapped_df_line(self):
        self.set_df(DF_WRAPPED)
        self.assertTrue(self.deploy(), self.deployer.deploy_failures)

    def test_df_missing_is_unknown(self):
        stub = self.stub_dir / "df"
        stub.write_text("#!/bin/sh\nexit 1\n")
        stub.chmod(0o755)
        self.assertTrue(self.deploy(), self.deployer.deploy_failures)


if __name__ == "__main__":
    unittest.main(verbosity=2)
