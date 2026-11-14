#!/usr/bin/env python3
"""remote/deploy_remote.py must not report success or restart the runtime after a failed upload.

The tool runs in-process with its ssh/scp calls replaced by a fake remote (no network):
files are "uploaded" into a dict, md5sum answers from it, and the restart command is
recorded. The ready-to-go check is not under test here and is replaced by True.
"""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
TOOL = REPO_DIR / "remote" / "deploy_remote.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("deploy_remote_under_test", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class FakeRemote:
    def __init__(self, scp_fails=(), mv_fails=()):
        self.files: dict[str, bytes] = {}
        self.scp_fails = set(scp_fails)
        self.mv_fails = set(mv_fails)
        self.commands: list[str] = []

    def run_cmd(self, cmd):
        tool = cmd[0]
        if tool == "scp":
            src, dst = cmd[-2], cmd[-1].split(":", 1)[1]
            if pathlib.Path(src).name in self.scp_fails:
                return subprocess.CompletedProcess(cmd, 1, "", "lost connection")
            self.files[dst] = pathlib.Path(src).read_bytes()
            return subprocess.CompletedProcess(cmd, 0, "", "")
        line = cmd[-1]
        self.commands.append(line)
        if line == "uname -a":
            return subprocess.CompletedProcess(cmd, 0, "Linux juniper armv5tejl\n", "")
        m = re.match(r"chmod \S+ (\S+)\.new.*mv -f \S+ (\S+)", line)
        if m:
            final = m.group(2)
            tmp = final + ".new"
            if pathlib.PurePosixPath(final).name in self.mv_fails or tmp not in self.files:
                return subprocess.CompletedProcess(cmd, 1, "", "mv: No space left on device")
            self.files[final] = self.files.pop(tmp)
            out = f"{hashlib.md5(self.files[final]).hexdigest()}  {final}\n" if "md5sum" in line else ""
            return subprocess.CompletedProcess(cmd, 0, out, "")
        if "init.sh stop" in line:
            return subprocess.CompletedProcess(cmd, 0, "RESTARTING\n", "")
        return subprocess.CompletedProcess(cmd, 0, "", "")

    @property
    def restarted(self) -> bool:
        return any("init.sh stop" in c for c in self.commands)


class DeployRemoteFailures(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.mod = load_tool()
        files = []
        for name in ("remote_ui", "remote_sync", "init.sh"):
            p = base / name
            p.write_bytes(f"content of {name}\n".encode())
            files.append(p)
        rcs = base / "rcS.local"
        rcs.write_text("#!/bin/sh\n")
        self.mod.DEPLOY_FILES = files
        self.mod.RCS_LOCAL_FILE = rcs
        self.mod.THEMES_DIR = base / "no_themes"
        self.mod.ready_to_go = lambda *a, **k: True

    def tearDown(self):
        self.tmp.cleanup()

    def deploy(self, remote: FakeRemote) -> tuple[int, str]:
        self.mod.run_cmd = remote.run_cmd
        out = io.StringIO()
        code = 0
        argv = sys.argv
        sys.argv = ["deploy_remote.py", "--no-sync"]
        try:
            with contextlib.redirect_stdout(out):
                self.mod.main()
        except SystemExit as e:
            code = int(e.code or 0)
        finally:
            sys.argv = argv
        return code, out.getvalue()

    def test_all_uploads_fail(self):
        remote = FakeRemote(scp_fails={"remote_ui", "remote_sync", "init.sh", "rcS.local"})
        code, out = self.deploy(remote)
        self.assertNotEqual(code, 0, out)
        self.assertNotIn("Deployment complete", out)
        self.assertFalse(remote.restarted, "the runtime must not be restarted after failed uploads")

    def test_install_step_fails(self):
        remote = FakeRemote(mv_fails={"remote_ui"})
        code, out = self.deploy(remote)
        self.assertNotEqual(code, 0, out)
        self.assertIn("remote_ui", out)
        self.assertFalse(remote.restarted)

    def test_good_deploy(self):
        remote = FakeRemote()
        code, out = self.deploy(remote)
        self.assertEqual(code, 0, out)
        self.assertIn("Deployment complete", out)
        self.assertTrue(remote.restarted)
        self.assertEqual(remote.files["/data/remote_ui"], b"content of remote_ui\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
