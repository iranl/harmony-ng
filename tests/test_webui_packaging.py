#!/usr/bin/env python3
"""Packaging checks of the two web UIs.

1. Both import the remote screen renderer with a version query.

The Hub serves everything under /assets/ as immutable for a year, and the renderer files
(remote_preview.js, remote_render.js/.wasm, themes) are copied there without a content hash.
An import without a version query keeps a browser on the first renderer it loaded, after any
update. Each UI must import assets/remote_preview.js with ?v=__REMOTE_RENDERER_VERSION__, and its
vite.config.js must define that constant.

2. Neither Android app allows Android backup of its data. The app keeps the Hub profiles, which
can hold the Hub password, and the WebView storage with the session token; Auto Backup and adb
backup would copy them off the phone.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMPORT_RE = re.compile(r"assets/remote_preview\.js([^'\"`)]*)")


def main():
    print("[-] Web UI packaging")
    failures = []
    for app in ("android-app", "frontend"):
        manifest = (ROOT / app / "android" / "app" / "src" / "main" / "AndroidManifest.xml").read_text()
        if 'android:allowBackup="false"' not in manifest:
            failures.append(f"{app}: AndroidManifest.xml allows Android backup of the app data")
    for app in ("android-app", "frontend"):
        config = (ROOT / app / "vite.config.js").read_text()
        if "__REMOTE_RENDERER_VERSION__" not in config:
            failures.append(f"{app}/vite.config.js does not define __REMOTE_RENDERER_VERSION__")
        found = 0
        for f in sorted((ROOT / app / "src").rglob("*.js")):
            for m in IMPORT_RE.finditer(f.read_text(errors="ignore")):
                found += 1
                if "?v=${__REMOTE_RENDERER_VERSION__}" not in m.group(1):
                    failures.append(f"{f.relative_to(ROOT)}: imports assets/remote_preview.js without the version")
        if not found:
            failures.append(f"{app}: no import of assets/remote_preview.js found (test out of date?)")
    for msg in failures:
        print(f"    FAIL {msg}")
    if failures:
        return 1
    print("    ok   both UIs import the renderer with its content hash")
    print("    ok   neither Android app allows backup of its data")
    return 0


if __name__ == "__main__":
    sys.exit(main())
