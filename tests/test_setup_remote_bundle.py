#!/usr/bin/env python3
"""remote/setup_remote.py step 2 must find init.sh and remote_watchdog.sh in the release bundle.

The remote release bundle has the tool as tools/setup_remote.py and the supervisor
scripts in the bundle root (.github/workflows/release.yml). The test stages that layout
in a temporary directory, runs step 2 from there in-process with every device call
replaced (no network), and checks which files were copied with scp.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent


class SetupRemoteBundle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        bundle = pathlib.Path(self.tmp.name) / "bundle"
        (bundle / "tools").mkdir(parents=True)
        (bundle / "bin").mkdir()
        shutil.copy(REPO_DIR / "remote" / "setup_remote.py", bundle / "tools" / "setup_remote.py")
        shutil.copy(REPO_DIR / "tools" / "usb_state.py", bundle / "tools" / "usb_state.py")
        for name in ("rcS.local", "init.sh", "remote_watchdog.sh"):
            shutil.copy(REPO_DIR / "remote" / name, bundle / name)
        shutil.copy(REPO_DIR / "remote" / "b64dec_tiny", bundle / "bin" / "b64dec_tiny")
        shutil.copy(REPO_DIR / "remote" / "bin" / "dropbearmulti_musl", bundle / "bin" / "dropbearmulti_musl")
        (bundle / "bin" / "remote_ui").write_bytes(b"\x7fELF fake")
        self.bundle = bundle
        self.pub = pathlib.Path(self.tmp.name) / "key.pub"
        self.pub.write_text("ssh-ed25519 AAAAtest user@test\n")
        self.copied: list[str] = []

    def tearDown(self):
        self.tmp.cleanup()

    def fake_run(self, cmd, *args, **kwargs):
        if cmd and cmd[0] == "scp":
            self.copied.append(pathlib.Path(cmd[-2]).name)
        return subprocess.CompletedProcess(cmd, 0, "Linux juniper armv5tejl\n" if kwargs.get("text") else b"", "")

    def test_scripts_found_in_bundle_root(self):
        tool = self.bundle / "tools" / "setup_remote.py"
        spec = importlib.util.spec_from_file_location("setup_remote_bundle_under_test", tool)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.assertEqual(mod.REPO_DIR, self.bundle)
        sizes = {"/tmp/tinysz.json": str(len(mod.TINY_PATH.read_bytes())),
                 "/tmp/dbsz.json": str(len(mod.DROPBEAR_PATH.read_bytes()))}
        with mock.patch.object(mod, "rce", lambda *a, **k: None), \
             mock.patch.object(mod, "read_json_val", lambda f, key, timeout=5: sizes[f]), \
             mock.patch.object(mod.time, "sleep", lambda s: None), \
             mock.patch.object(mod.subprocess, "run", self.fake_run), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(mod.step2_install_permanent_ssh(self.pub, no_backup=True))
        for name in ("remote_ui", "rcS.local", "init.sh", "remote_watchdog.sh"):
            self.assertIn(name, self.copied, f"{name} was not uploaded from the bundle layout")


if __name__ == "__main__":
    unittest.main(verbosity=2)
