#!/usr/bin/env python3
"""build/deploy_hub.py must start the Hub services again when it ends after stopping them.

The deployer runs in-process with its ssh commands recorded (no network), uploads
reported as done, and npm replaced: a failed web UI build and a missing npm both end
the deployment after the services were stopped. The services must then be started again.
When pidof finds no service at the end, the run must not report success.
"""

from __future__ import annotations

import contextlib
import io
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))

import deploy_hub  # noqa: E402


class DeployHubRestart(unittest.TestCase):
    def make(self, pidof: str = "101 102 103", ui: str = "new"):
        d = deploy_hub.HubDeployer(hub_host="hub.invalid", ssh_key="/nonexistent", ui_choice=ui, no_backup=True)
        self.ssh: list[str] = []

        def fake_ssh(cmd: str) -> subprocess.CompletedProcess[str]:
            self.ssh.append(cmd)
            if cmd.startswith("pidof"):
                return subprocess.CompletedProcess(cmd, 0, pidof, "")
            if "devices.json ] && echo 1" in cmd:
                return subprocess.CompletedProcess(cmd, 0, "1\n", "")
            return subprocess.CompletedProcess(cmd, 0, "", "")

        d.run_ssh = fake_ssh
        d.deploy_file_if_changed = lambda *a, **k: True
        # Repository layout with the web UI built from frontend/
        d.www_dir = d.repo_root / "payload" / "www"
        return d

    def stopped_then_started(self) -> tuple[bool, bool]:
        stop = next((i for i, c in enumerate(self.ssh) if "init.sh stop" in c and "touch /tmp/harmony_stop" in c), None)
        start = next((i for i, c in enumerate(self.ssh) if "rm -f /tmp/harmony_stop" in c and "init.sh restart" in c), None)
        return stop is not None, start is not None and stop is not None and start > stop

    def run_deploy(self, d, npm) -> tuple[object, str]:
        def fake_run(cmd, *args, **kwargs):
            if cmd and cmd[0] in ("npm", "npm.cmd"):
                return npm(cmd)
            if cmd and cmd[0] in ("scp", "ssh"):
                return subprocess.CompletedProcess(cmd, 0, "", "")
            # nothing else runs for real (no firmware download, no theme build)
            return subprocess.CompletedProcess(cmd, 1, "", "not run in this test")

        out = io.StringIO()
        result: object = None
        with mock.patch.object(deploy_hub.subprocess, "run", side_effect=fake_run), \
             mock.patch.object(deploy_hub.shutil, "which", return_value=None), \
             mock.patch.object(deploy_hub.time, "sleep", lambda s: None), \
             mock.patch("builtins.input", return_value="n"), \
             contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            try:
                result = d.deploy()
            except Exception as e:  # noqa: BLE001
                result = e
        return result, out.getvalue()

    def test_webui_build_fails(self):
        d = self.make()
        result, out = self.run_deploy(d, lambda cmd: subprocess.CompletedProcess(cmd, 1, "", ""))
        self.assertFalse(result is True, out)
        stopped, started = self.stopped_then_started()
        self.assertTrue(stopped, "precondition: the services were stopped")
        self.assertTrue(started, "services left stopped after a failed web UI build")

    def test_npm_missing(self):
        d = self.make()

        def no_npm(cmd):
            raise FileNotFoundError(2, "No such file or directory", "npm")

        result, out = self.run_deploy(d, no_npm)
        self.assertFalse(result is True, out)
        stopped, started = self.stopped_then_started()
        self.assertTrue(stopped)
        self.assertTrue(started, "services left stopped when npm is missing")

    def test_no_service_running_is_not_success(self):
        d = self.make(pidof="", ui="legacy")
        d.www_dir = REPO_DIR / "nonexistent_www"  # no web UI build or upload in this case
        result, out = self.run_deploy(d, lambda cmd: subprocess.CompletedProcess(cmd, 0, "", ""))
        self.assertIsNot(result, True, out)
        self.assertNotIn("Services started", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
