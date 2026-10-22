#!/usr/bin/env python3
"""
bump_version.py - Single Source of Truth Version Management

Synchronizes version number across:
  - VERSION (root file)
  - payload/source/http_server.c (CODEX_VERSION)
  - android-app/src/app.js (APP_VERSION)
  - android-app/package.json ("version")
  - android-app/android/app/build.gradle (versionName, versionCode)
"""

import sys
import re
import argparse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

VERSION_FILE = REPO_ROOT / "VERSION"
CODEX_VERSION_H = REPO_ROOT / "payload" / "source" / "codex_version.h"
APP_JS = REPO_ROOT / "android-app" / "src" / "app.js"
PACKAGE_JSON = REPO_ROOT / "android-app" / "package.json"
BUILD_GRADLE = REPO_ROOT / "android-app" / "android" / "app" / "build.gradle"

def get_current_version():
    if VERSION_FILE.exists():
        return VERSION_FILE.read_text().strip()
    return "1.0.0"

def bump_semver(ver_str, part="patch"):
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)(.*)$", ver_str)
    if not m:
        raise ValueError(f"Invalid semver: {ver_str}")
    major, minor, patch, extra = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
    if part == "major":
        major += 1
        minor = 0
        patch = 0
    elif part == "minor":
        minor += 1
        patch = 0
    elif part == "patch":
        patch += 1
    return f"{major}.{minor}.{patch}{extra}"

def update_file(path: Path, pattern: str, replacement: str):
    if not path.exists():
        print(f"[-] Skipping missing file: {path}")
        return False
    content = path.read_text(encoding="utf-8")
    new_content, count = re.subn(pattern, replacement, content)
    if count == 0:
        print(f"[-] Warning: Pattern '{pattern}' not matched in {path}")
        return False
    path.write_text(new_content, encoding="utf-8")
    print(f"[+] Updated {path.relative_to(REPO_ROOT)} ({count} match)")
    return True

def sync_all(new_version, increment_vc=False):
    print(f"[*] Synchronizing project version to {new_version}...")

    # 1. Root VERSION
    VERSION_FILE.write_text(new_version.strip() + "\n", encoding="utf-8")
    print(f"[+] Updated VERSION -> {new_version}")

    # 2. codex_version.h
    update_file(
        CODEX_VERSION_H,
        r'#define\s+CODEX_VERSION\s+"[^"]+"',
        f'#define CODEX_VERSION "{new_version}"'
    )

    # 3. app.js
    update_file(
        APP_JS,
        r"export\s+const\s+APP_VERSION\s*=\s*'[^']+';",
        f"export const APP_VERSION = '{new_version}';"
    )

    # 4. package.json
    update_file(
        PACKAGE_JSON,
        r'"version"\s*:\s*"[^"]+"',
        f'"version": "{new_version}"'
    )

    # 5. build.gradle (versionName & optional versionCode increment)
    if BUILD_GRADLE.exists():
        content = BUILD_GRADLE.read_text(encoding="utf-8")
        vc_match = re.search(r'versionCode\s+(\d+)', content)
        new_vc = 1
        if vc_match:
            cur_vc = int(vc_match.group(1))
            new_vc = cur_vc + 1 if increment_vc else cur_vc
        content = re.sub(r'versionCode\s+\d+', f'versionCode {new_vc}', content)
        content = re.sub(r'versionName\s+"[^"]+"', f'versionName "{new_version}"', content)
        BUILD_GRADLE.write_text(content, encoding="utf-8")
        print(f"[+] Updated {BUILD_GRADLE.relative_to(REPO_ROOT)} (versionName='{new_version}', versionCode={new_vc})")

    print(f"[+] Successfully bumped and synced all components to v{new_version}!")

def main():
    parser = argparse.ArgumentParser(description="Synchronize version across Harmony project")
    parser.add_argument("--version", "-v", help="Explicit version string (e.g. 1.3.0)")
    parser.add_argument("--bump", "-b", choices=["patch", "minor", "major"], help="Bump semver part")
    parser.add_argument("--sync", "-s", action="store_true", help="Sync from current VERSION file")
    args = parser.parse_args()

    cur = get_current_version()
    print(f"[*] Current repository version: {cur}")

    increment = False
    if args.version:
        target = args.version.lstrip("v")
        increment = True
    elif args.bump:
        target = bump_semver(cur, args.bump)
        increment = True
    elif args.sync:
        target = cur
        increment = False
    else:
        parser.print_help()
        sys.exit(1)

    sync_all(target, increment_vc=increment)

if __name__ == "__main__":
    main()
