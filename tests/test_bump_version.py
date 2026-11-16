#!/usr/bin/env python3
"""scripts/bump_version.py on a scratch copy of the version files.

- A version file that moved, or whose version line changed, is an error (exit code 1), not
  a "[-]" line among "[+]" lines with exit code 0.
- The root package's version in android-app/package-lock.json follows; dependency versions
  do not change.
- --next prints the next version and changes nothing, so the release workflow raises
  versionCode once per release (it was raised twice in the repository, once in the APK).
"""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
FILES = ["VERSION", "payload/source/harmony_version.h", "docs/openapi.yaml",
         "android-app/src/app/app_icons_ui.js", "android-app/package.json",
         "android-app/package-lock.json", "android-app/android/app/build.gradle",
         "scripts/bump_version.py"]


class BumpVersion(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for f in FILES:
            dst = self.root / f
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO_DIR / f, dst)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(self.root / "scripts" / "bump_version.py"), *args],
                              capture_output=True, text=True, timeout=60)

    def version_code(self) -> int:
        text = (self.root / "android-app/android/app/build.gradle").read_text()
        return int(re.search(r"versionCode\s+(\d+)", text).group(1))

    def test_moved_file_is_an_error(self):
        (self.root / "android-app/src/app/app_icons_ui.js").unlink()
        res = self.run_tool("--version", "9.9.9")
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertIn("app_icons_ui.js", res.stdout)
        self.assertNotIn("Successfully", res.stdout)

    def test_changed_pattern_is_an_error(self):
        h = self.root / "payload/source/harmony_version.h"
        h.write_text(h.read_text().replace("#define HARMONY_VERSION ", "#define HARMONY_VER "))
        self.assertNotEqual(self.run_tool("--sync").returncode, 0)

    def test_lockfile_root_version(self):
        lock = self.root / "android-app/package-lock.json"
        deps_before = re.findall(r'"node_modules/[^"]+":\s*\{\s*"version":\s*"[^"]+"', lock.read_text())
        self.assertEqual(self.run_tool("--version", "9.9.9").returncode, 0)
        text = lock.read_text()
        self.assertRegex(text, r'\A\{\s*"name":\s*"[^"]*",\s*"version":\s*"9\.9\.9"')
        self.assertRegex(text, r'"":\s*\{\s*"name":\s*"[^"]*",\s*"version":\s*"9\.9\.9"')
        self.assertEqual(re.findall(r'"node_modules/[^"]+":\s*\{\s*"version":\s*"[^"]+"', text), deps_before)

    def test_release_raises_version_code_once(self):
        before = self.version_code()
        version_file = (self.root / "VERSION").read_text()
        nxt = self.run_tool("--next", "patch")
        self.assertEqual(nxt.returncode, 0)
        self.assertRegex(nxt.stdout.strip(), r"^\d+\.\d+\.\d+$")
        self.assertEqual((self.root / "VERSION").read_text(), version_file, "--next changed a file")
        self.assertEqual(self.version_code(), before)
        self.assertEqual(self.run_tool("--version", nxt.stdout.strip()).returncode, 0)
        self.assertEqual(self.version_code(), before + 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
