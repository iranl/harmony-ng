#!/usr/bin/env python3
"""build/gzip_assets.py must give the same .gz for the same input.

deploy_hub.py compares MD5s and uploads every web UI file whose .gz changed. Before
Python 3.14, gzip.compress() writes the current time into the header unless mtime is
given, so every rebuild changed every file. The test runs the script with gzip.compress
behaving as on Python 3.13 (time in the header by default) and checks that the header
time field is 0 and that two runs give identical files.
"""

from __future__ import annotations

import contextlib
import gzip
import importlib.util
import io
import pathlib
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "build" / "gzip_assets.py"
REAL_COMPRESS = gzip.compress


def compress_like_py313(data, compresslevel=9, *, mtime=None):
    return REAL_COMPRESS(data, compresslevel, mtime=time.time() if mtime is None else mtime)


def run_script(www: pathlib.Path) -> dict[str, bytes]:
    spec = importlib.util.spec_from_file_location("gzip_assets_under_test", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    with mock.patch.object(mod.gzip, "compress", compress_like_py313), \
         mock.patch.object(sys, "argv", ["gzip_assets.py", str(www)]), \
         contextlib.redirect_stdout(io.StringIO()):
        mod.main()
    return {str(p.relative_to(www)): p.read_bytes() for p in sorted(www.rglob("*.gz"))}


def make_www(base: pathlib.Path) -> pathlib.Path:
    www = base / "www"
    (www / "assets").mkdir(parents=True)
    (www / "index.html").write_text("<html><body>Harmony</body></html>\n")
    (www / "assets" / "app.js").write_text("console.log('x');\n" * 50)
    return www


class GzipAssetsReproducible(unittest.TestCase):
    def test_same_input_same_output(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            first = run_script(make_www(pathlib.Path(a)))
            time.sleep(1.1)
            second = run_script(make_www(pathlib.Path(b)))
        self.assertEqual(sorted(first), ["assets/app.js.gz", "index.html.gz"])
        for name, data in first.items():
            self.assertEqual(data[4:8], b"\0\0\0\0", f"{name}: build time in the gzip header")
            self.assertEqual(data, second[name], f"{name}: two builds of the same file differ")


if __name__ == "__main__":
    unittest.main(verbosity=2)
