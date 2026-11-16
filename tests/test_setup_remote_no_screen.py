#!/usr/bin/env python3
"""remote/setup_remote.py --no-screen must write the flag where remote_ui reads it.

Step 2 runs in-process with every device call replaced (no network): the port 8088
commands are dropped, the size checks answer the expected sizes, and each ssh command is
recorded. The recorded commands then run with sh against a temporary /data, and the file
that remote_ui reads (SETTINGS_CONFIG_FILE in remote_state.h) must hold no_screen=1.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import pathlib
import re
import subprocess
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
TOOL = REPO_DIR / "remote" / "setup_remote.py"
HEADER = REPO_DIR / "payload" / "source" / "remote" / "remote_state.h"


def settings_file() -> str:
    m = re.search(r'#define\s+SETTINGS_CONFIG_FILE\s+"([^"]+)"', HEADER.read_text())
    assert m, "SETTINGS_CONFIG_FILE not found in remote_state.h"
    return m.group(1)


def load_tool():
    spec = importlib.util.spec_from_file_location("setup_remote_under_test", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class SetupRemoteNoScreen(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        self.data = self.base / "data"
        self.data.mkdir()
        self.mod = load_tool()
        self.ssh_cmds: list[str] = []
        pub = self.base / "key.pub"
        pub.write_text("ssh-ed25519 AAAAtest user@test\n")
        self.pub = pub

    def tearDown(self):
        self.tmp.cleanup()

    def fake_run(self, cmd, *args, **kwargs):
        if cmd and cmd[0] == "ssh":
            self.ssh_cmds.append(cmd[-1])
        return subprocess.CompletedProcess(cmd, 0, "Linux juniper armv5tejl\n" if kwargs.get("text") else b"", "")

    def run_step2(self, no_screen: bool) -> None:
        mod = self.mod
        sizes = {"/tmp/tinysz.json": str(len(mod.TINY_PATH.read_bytes())),
                 "/tmp/dbsz.json": str(len(mod.DROPBEAR_PATH.read_bytes()))}
        with mock.patch.object(mod, "rce", lambda *a, **k: None), \
             mock.patch.object(mod, "read_json_val", lambda f, key, timeout=5: sizes[f]), \
             mock.patch.object(mod.time, "sleep", lambda s: None), \
             mock.patch.object(mod.subprocess, "run", self.fake_run), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(mod.step2_install_permanent_ssh(self.pub, no_screen=no_screen, no_backup=True))

    def replay_on_host(self) -> None:
        """Run the recorded ssh commands that touch a settings file, with /data moved."""
        for c in self.ssh_cmds:
            if "no_screen" not in c:
                continue
            local = c.replace("/data/", f"{self.data}/")
            subprocess.run(["sh", "-c", local], check=False, timeout=10)

    def test_no_screen_reaches_remote_ui(self):
        self.run_step2(no_screen=True)
        self.replay_on_host()
        conf = self.data / pathlib.PurePosixPath(settings_file()).name
        self.assertTrue(conf.is_file(), f"{settings_file()} was not written; files: {sorted(p.name for p in self.data.iterdir())}")
        self.assertIn("no_screen=1", conf.read_text().split())

    def test_existing_settings_kept(self):
        conf = self.data / pathlib.PurePosixPath(settings_file()).name
        conf.write_text("brightness=3\nno_screen=0\n")
        self.run_step2(no_screen=True)
        self.replay_on_host()
        lines = conf.read_text().split()
        self.assertIn("brightness=3", lines)
        self.assertEqual([x for x in lines if x.startswith("no_screen=")], ["no_screen=1"])

    def test_without_option_nothing_written(self):
        self.run_step2(no_screen=False)
        self.assertFalse([c for c in self.ssh_cmds if "no_screen" in c])


if __name__ == "__main__":
    unittest.main(verbosity=2)
