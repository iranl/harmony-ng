#!/usr/bin/env python3
"""Legacy web UI modules must import the exported functions they call.

android-app/src is plain ES modules: a function exported by one module and called in
another without an import is a ReferenceError at the moment the code runs (for example
the remote slot editor's Save and Delete buttons), which no build step reports. The test
lists every exported function and fails for a module that calls one of them by name
without importing or defining it.
"""

from __future__ import annotations

import pathlib
import re
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SRC = REPO_DIR / "android-app" / "src"


def exported_functions() -> dict[str, set[pathlib.Path]]:
    names: dict[str, set[pathlib.Path]] = {}
    for f in SRC.rglob("*.js"):
        for m in re.finditer(r"^export\s+(?:async\s+)?function\s+(\w+)", f.read_text(), re.M):
            names.setdefault(m.group(1), set()).add(f)
    return names


class UiImports(unittest.TestCase):
    def test_exported_functions_are_imported_where_called(self):
        exports = exported_functions()
        self.assertIn("saveRemoteLayout", exports)
        missing = []
        for f in sorted(SRC.rglob("*.js")):
            text = f.read_text()
            for name, sources in exports.items():
                if f in sources:
                    continue
                # a bare call, not a method call (obj.name() or this.name())
                if not re.search(r"(?<![\w.$])" + name + r"\s*\(", text):
                    continue
                imported = re.search(r"import\s*\{[^}]*\b" + name + r"\b[^}]*\}\s*from", text)
                defined = re.search(r"(function\s+" + name + r"\b|(?:const|let|var)\s+" + name + r"\b|^\s*(?:async\s+)?"
                                    + name + r"\s*\([^)]*\)\s*\{)", text, re.M)
                if not imported and not defined:
                    missing.append(f"{f.relative_to(REPO_DIR)}: {name}()")
        self.assertEqual(missing, [], "functions called without an import (ReferenceError at run time)")


if __name__ == "__main__":
    unittest.main(verbosity=2)
