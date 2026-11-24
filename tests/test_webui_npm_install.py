#!/usr/bin/env python3
"""build/build_webui.py and build/deploy_hub.py install the web UI's packages before building.

A fresh clone or worktree has no node_modules, and `npm run build` then stopped at
"vite: not found". Now `npm ci` runs first when node_modules is missing or was installed from
another package-lock.json (content compared, not dates; `npm install` without a lock file),
and a failed install stops the build.
npm is a fake: nothing is installed or built.
"""

from __future__ import annotations

import contextlib
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))

import build_webui  # noqa: E402


class FakeNpm:
    """subprocess.run stand-in: records npm calls, answers with the given exit codes."""

    def __init__(self, ci_rc: int = 0, build_rc: int = 1, app: pathlib.Path | None = None):
        self.calls: list[list[str]] = []
        self.app = app  # when set, a successful install creates node_modules like npm does
        self.ci_rc = ci_rc
        self.build_rc = build_rc

    def __call__(self, cmd, *args, **kwargs):
        self.calls.append(list(cmd[1:]))
        rc = self.build_rc if cmd[1:3] == ["run", "build"] else self.ci_rc
        if self.app is not None and rc == 0 and cmd[1:2] in (["ci"], ["install"]):
            (self.app / "node_modules").mkdir(exist_ok=True)
            (self.app / "node_modules" / ".package-lock.json").write_text("{}")
        return subprocess.CompletedProcess(cmd, rc, "", "")


class EnsureNodeModulesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.app = pathlib.Path(self.tmp.name) / "android-app"
        self.app.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def lock(self, mtime: float = 1000, text: str = "{}"):
        p = self.app / "package-lock.json"
        p.write_text(text)
        os.utime(p, (mtime, mtime))

    def installed(self, mtime: float = 2000):
        p = self.app / "node_modules" / ".package-lock.json"
        p.parent.mkdir(exist_ok=True)
        p.write_text("{}")
        os.utime(p, (mtime, mtime))

    def run_ensure(self, npm: FakeNpm) -> bool:
        with mock.patch.object(build_webui.subprocess, "run", side_effect=npm), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return build_webui.ensure_node_modules(self.app)

    def test_missing_node_modules_runs_npm_ci(self):
        self.lock()
        npm = FakeNpm()
        self.assertTrue(self.run_ensure(npm))
        self.assertEqual(npm.calls, [["ci"]])

    def test_installed_and_current_runs_nothing(self):
        self.lock(1000)
        self.installed(2000)
        npm = FakeNpm()
        self.assertTrue(self.run_ensure(npm))
        self.assertEqual(npm.calls, [])

    def test_lock_newer_than_install_runs_npm_ci(self):
        self.lock(3000)
        self.installed(2000)
        npm = FakeNpm()
        self.assertTrue(self.run_ensure(npm))
        self.assertEqual(npm.calls, [["ci"]])

    def test_no_lock_file_runs_npm_install(self):
        npm = FakeNpm()
        self.assertTrue(self.run_ensure(npm))
        self.assertEqual(npm.calls, [["install"]])

    def test_failed_install_is_reported(self):
        self.lock()
        self.assertFalse(self.run_ensure(FakeNpm(ci_rc=1)))

    def test_same_lock_rewritten_installs_nothing(self):
        # a checkout or rebase rewrites package-lock.json with the same content and a new date
        self.lock(1000)
        npm = FakeNpm(app=self.app)
        self.assertTrue(self.run_ensure(npm))
        self.assertEqual(npm.calls, [["ci"]])
        self.lock(5000)
        os.utime(self.app / "node_modules" / ".package-lock.json", (2000, 2000))
        npm = FakeNpm(app=self.app)
        self.assertTrue(self.run_ensure(npm))
        self.assertEqual(npm.calls, [])

    def test_changed_lock_content_runs_npm_ci(self):
        self.lock(1000)
        self.assertTrue(self.run_ensure(FakeNpm(app=self.app)))
        self.lock(1000, text='{"lockfileVersion": 3}')  # new content, even with an older date
        os.utime(self.app / "node_modules" / ".package-lock.json", (2000, 2000))
        npm = FakeNpm(app=self.app)
        self.assertTrue(self.run_ensure(npm))
        self.assertEqual(npm.calls, [["ci"]])

    def test_install_without_stamp_records_it(self):
        # node_modules installed by an earlier version of this check: dates decide once
        self.lock(1000)
        self.installed(2000)
        self.assertTrue(self.run_ensure(FakeNpm()))
        stamp = self.app / "node_modules" / build_webui.LOCK_STAMP
        self.assertTrue(stamp.is_file())
        self.lock(9000)  # same content, newer date
        npm = FakeNpm()
        self.assertTrue(self.run_ensure(npm))
        self.assertEqual(npm.calls, [])

    def test_build_webui_installs_before_building(self):
        self.lock()
        npm = FakeNpm(build_rc=1)  # the build itself fails: only the order matters here
        with mock.patch.object(build_webui.subprocess, "run", side_effect=npm), \
             mock.patch.object(build_webui.shutil, "which", return_value=None), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            build_webui.build_webui("legacy", pathlib.Path(self.tmp.name))
        self.assertEqual(npm.calls, [["ci"], ["run", "build"]])

    def test_build_webui_stops_when_install_fails(self):
        self.lock()
        npm = FakeNpm(ci_rc=1)
        with mock.patch.object(build_webui.subprocess, "run", side_effect=npm), \
             mock.patch.object(build_webui.shutil, "which", return_value=None), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(build_webui.build_webui("legacy", pathlib.Path(self.tmp.name)))
        self.assertEqual(npm.calls, [["ci"]])


class DeployHubTest(unittest.TestCase):
    def test_deploy_hub_installs_before_building(self):
        source = (REPO_DIR / "build" / "deploy_hub.py").read_text()
        ensure = source.find("build_webui.ensure_node_modules(ui_src_dir)")
        build = source.find('"run", "build"]')
        self.assertGreater(ensure, 0, "deploy_hub.py does not install the packages")
        self.assertGreater(build, ensure, "deploy_hub.py builds before installing the packages")


if __name__ == "__main__":
    unittest.main(verbosity=1)
