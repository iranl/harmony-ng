#!/usr/bin/env python3
"""tools/remote_render/build_theme.py: the download cache follows the pinned version.

The cache was keyed by source id and icon name, so after a version bump in icon_sets.json a
machine with a cache kept building with the old files while a clean CI machine used the new
ones. urlopen is replaced (no network). Also: packages of themes that are no longer in
remote/themes are removed from the output folder when every theme is built.
"""

from __future__ import annotations

import importlib.util
import io
import pathlib
import tempfile
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent


def load_tool():
    spec = importlib.util.spec_from_file_location("build_theme", REPO_DIR / "tools" / "remote_render" / "build_theme.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class BuildThemeCache(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = pathlib.Path(self.tmp.name)
        self.mod = load_tool()
        self.fetched: list[str] = []

    def tearDown(self):
        self.tmp.cleanup()

    def fake_urlopen(self, url, timeout=30):
        self.fetched.append(url)
        return io.BytesIO(f"<svg><!-- {url} --></svg>".encode())

    def download(self, version: str) -> str:
        cfg = {"sources": {"set": {"url": f"https://cdn.example/icons@{version}/{{name}}.svg"}}}
        builder = self.mod.Builder(cfg, str(self.work), "unused")
        with mock.patch.object(self.mod.urllib.request, "urlopen", self.fake_urlopen):
            path, _ = builder.download("set", "television")
        return pathlib.Path(path).read_text()

    def test_version_bump_fetches_the_new_file(self):
        self.assertIn("icons@2.1.1", self.download("2.1.1"))
        self.assertIn("icons@2.2.0", self.download("2.2.0"), "the old version's file was used after the bump")
        self.assertEqual(len(self.fetched), 2)
        self.download("2.2.0")
        self.assertEqual(len(self.fetched), 2, "a cached file was downloaded again")

    def test_removed_theme_leaves_the_output(self):
        out = self.work / "out"
        for name in ("dark", "gone"):
            (out / name).mkdir(parents=True)
        self.mod.remove_stale_packages(str(out), ["dark"])
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["dark"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
