#!/usr/bin/env python3
"""Every Hub path the two web UIs call has a handler in the Hub sources.

The UIs had pages whose buttons always failed with 404 because the route they posted to never
existed (Harmony DB search and import, IR Lab import and batch run, Bluetooth macro script).
This collects each path literal of the form /api/..., /ir/..., /bt/..., /wifi, /export, /import
from android-app/src and frontend/src and looks for it in payload/source: as a quoted path
("/api/x", or a prefix "/api/x/ for routes matched with strncmp) or after a method in hub_rf's
request line match ("POST /api/rf/x").
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
UI_DIRS = [ROOT / "android-app" / "src", ROOT / "frontend" / "src"]
PATH_RE = re.compile(r"""['"`](/(?:api|ir|bt|wifi|export|import)(?:[/-][A-Za-z0-9_\-]+|[A-Za-z0-9_\-]*)*)""")


def hub_source_text():
    parts = []
    for f in sorted((ROOT / "payload" / "source").rglob("*.c")):
        parts.append(f.read_text(errors="ignore"))
    return "\n".join(parts)


def handled(path, src):
    p = path.rstrip("/")
    if not p:
        return True
    pat = re.compile(r'(?:"|\b(?:GET|POST|PUT|DELETE) )' + re.escape(p) + r'(?:["?/ ])')
    return bool(pat.search(src))


def main():
    src = hub_source_text()
    missing = {}
    for d in UI_DIRS:
        for f in sorted(d.rglob("*.js")):
            for m in PATH_RE.finditer(f.read_text(errors="ignore")):
                path = m.group(1)
                if not handled(path, src):
                    missing.setdefault(path.rstrip("/"), set()).add(str(f.relative_to(ROOT)))
    print("[-] Web UI routes against the Hub's handlers")
    if missing:
        for path, files in sorted(missing.items()):
            print(f"    FAIL {path}: no handler (called from {', '.join(sorted(files))})")
        print(f"[-] {len(missing)} route(s) without a handler")
        return 1
    print("    ok   every route the web UIs call has a handler")
    return 0


if __name__ == "__main__":
    sys.exit(main())
