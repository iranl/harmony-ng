#!/usr/bin/env python3
"""build/build_webui.py, build_apk.py and deploy_hub.py share one web UI packaging step.

- payload/www is replaced as a whole: old hashed bundles, a theme removed from the project
  and the other UI's files do not stay next to the new build;
- a failed gzip, WebAssembly or theme step is a failure, not "built successfully";
- building the APK leaves payload/www (the Hub's web UI) alone.
No npm, no emcc: the tools' subprocess calls are replaced where needed.
"""

from __future__ import annotations

import contextlib
import io
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))

import build_apk  # noqa: E402
import build_webui  # noqa: E402


class WebuiSync(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "build").mkdir()
        shutil.copy(REPO_DIR / "build" / "gzip_assets.py", self.root / "build")
        self.dist = self.root / "app" / "dist"
        (self.dist / "assets").mkdir(parents=True)
        (self.dist / "index.html").write_text("<html>new</html>")
        (self.dist / "assets" / "index-new.js").write_text("console.log('new')")
        self.www = self.root / "payload" / "www"
        (self.www / "assets" / "themes").mkdir(parents=True)
        (self.www / "assets" / "index-old.js.gz").write_bytes(b"old")
        (self.www / "assets" / "themes" / "removed.tar").write_bytes(b"old theme")
        (self.www / "other-ui.html.gz").write_bytes(b"other")

    def tearDown(self):
        self.tmp.cleanup()

    def files(self) -> list[str]:
        return sorted(str(p.relative_to(self.www)) for p in self.www.rglob("*") if p.is_file())

    def test_www_replaced_as_a_whole(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(build_webui.sync_dist_to_www(self.dist, self.www, self.root))
        self.assertEqual(self.files(), ["assets/index-new.js.gz", "index.html.gz"])

    def test_gzip_failure_is_a_failure(self):
        (self.root / "build" / "gzip_assets.py").write_text("import sys\nsys.exit(3)\n")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(build_webui.sync_dist_to_www(self.dist, self.www, self.root))

    def test_theme_failure_is_a_failure(self):
        (self.root / "tools" / "remote_render").mkdir(parents=True)
        for f in ("build.sh", "build_theme.py"):
            (self.root / "tools" / "remote_render" / f).write_text("")

        def fake_run(cmd, *a, **k):
            rc = 1 if "build_theme.py" in " ".join(map(str, cmd)) else 0
            return subprocess.CompletedProcess(cmd, rc, "", "theme download failed")

        with mock.patch.object(build_webui.shutil, "which", return_value="/usr/bin/emcc"), \
             mock.patch.object(build_webui.subprocess, "run", side_effect=fake_run), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(build_webui.build_extras(self.root / "app", self.root))

    def test_apk_build_leaves_payload_www(self):
        app = self.root / "android-app"
        shutil.copytree(self.dist, app / "dist")
        apk_dir = app / "android" / "app" / "build" / "outputs" / "apk" / "debug"
        apk_dir.mkdir(parents=True)
        (apk_dir / "app-debug.apk").write_bytes(b"apk")
        (app / "android" / "gradlew").write_text("")
        before = self.files()
        with mock.patch.object(build_apk.subprocess, "run",
                               side_effect=lambda cmd, *a, **k: subprocess.CompletedProcess(cmd, 0, "", "")), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(build_apk.build_apk(repo_root=self.root))
        self.assertEqual(self.files(), before, "building the APK changed the Hub's web UI")


if __name__ == "__main__":
    unittest.main(verbosity=2)
