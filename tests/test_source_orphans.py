#!/usr/bin/env python3
"""Every C file directly in payload/source must be built by something.

payload/source holds the daemons' sources. A stand-alone program left there that no
build script or test compiles looks like daemon code to a reader, and keeps old protocol
steps nobody maintains. The test fails for a payload/source/*.c whose name appears in
none of the build scripts and test runners.
"""

from __future__ import annotations

import pathlib
import re
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent


class SourceOrphans(unittest.TestCase):
    def test_every_source_is_built(self):
        refs = ""
        for pattern in ("build/*.sh", "build/*.py", "tests/run_mock_suite.sh", "tests/*.py"):
            for f in REPO_DIR.glob(pattern):
                if f.resolve() != pathlib.Path(__file__).resolve():
                    refs += f.read_text(errors="replace")
        orphans = [f.name for f in sorted((REPO_DIR / "payload" / "source").glob("*.c"))
                   if not re.search(r"\b" + re.escape(f.stem) + r"\b", refs)]
        self.assertEqual(orphans, [], "payload/source files that no build script or test compiles")


if __name__ == "__main__":
    unittest.main(verbosity=2)
