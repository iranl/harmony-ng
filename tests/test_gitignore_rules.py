#!/usr/bin/env python3
"""
.gitignore ignores the local work folders at the top of the checkout only.

Rules such as `data/` or `release/` without a leading slash match a folder of that name at any
depth, so a new source folder like android-app/src/data/ or tests/fixtures/data/ would be left
out of every commit while it keeps working in the author's checkout. Uses
`git check-ignore --no-index`, so the paths need not exist. Skipped outside a git checkout.
"""

import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (path, ignored?)
CASES = [
    ("android-app/src/data/x.json", False),
    ("frontend/src/data/x.json", False),
    ("tests/fixtures/data/x", False),
    ("docs/release/x.md", False),
    ("frontend/src/artifacts/x", False),
    ("payload/firmware/x", False),
    ("android-app/src/background/x.js", False),
    ("tools/backups/x.py", False),
    (".agents/rules/x.md", False),
    ("data/x", True),
    ("release/x", True),
    ("backups/x.tar", True),
    ("firmware/x.bin", True),
    ("artifacts/x", True),
    ("background/x", True),
    ("android-app/dist/index.html", True),
    ("frontend/dist/index.html", True),
    (".agents/notes.md", True),
    ("build/toolchains", True),
]


def ignored(path):
    res = subprocess.run(["git", "-C", REPO_ROOT, "check-ignore", "--no-index", "-q", path],
                         capture_output=True)
    if res.returncode not in (0, 1):
        raise RuntimeError(res.stderr.decode())
    return res.returncode == 0


def main():
    if not shutil.which("git") or subprocess.run(["git", "-C", REPO_ROOT, "rev-parse"],
                                                  capture_output=True).returncode != 0:
        print("[-] not a git checkout, .gitignore check skipped")
        return
    failed = 0
    for path, want in CASES:
        got = ignored(path)
        if got == want:
            print("  PASS %s %s" % (path, "ignored" if want else "kept"))
        else:
            failed += 1
            print("  FAIL %s is %s" % (path, "ignored" if got else "not ignored"))
    if failed:
        print("[-] %d .gitignore check(s) failed" % failed)
        sys.exit(1)
    print("[+] .gitignore: top-level work folders only")


if __name__ == "__main__":
    main()
