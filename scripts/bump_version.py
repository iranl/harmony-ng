#!/usr/bin/env python3
"""
bump_version.py - Single Source of Truth Version Management

Synchronizes version number across:
  - VERSION (root file)
  - payload/source/harmony_version.h (HARMONY_VERSION)
  - android-app/src/app/app_icons_ui.js (APP_VERSION)
  - docs/openapi.yaml (info.version)
  - android-app/package.json ("version")
  - android-app/package-lock.json (the root package's "version")
  - android-app/android/app/build.gradle (versionName, versionCode)

A file that is missing or whose version line is not found is an error (exit code 1): it
would otherwise keep its old version while CI's "--sync, then git diff" check sees no
change. --next prints the next version without changing anything.
"""

import sys
import re
import argparse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

VERSION_FILE = REPO_ROOT / "VERSION"
HARMONY_VERSION_H = REPO_ROOT / "payload" / "source" / "harmony_version.h"
OPENAPI_YAML = REPO_ROOT / "docs" / "openapi.yaml"
APP_JS = REPO_ROOT / "android-app" / "src" / "app" / "app_icons_ui.js"
PACKAGE_JSON = REPO_ROOT / "android-app" / "package.json"
PACKAGE_LOCK = REPO_ROOT / "android-app" / "package-lock.json"
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

FAILURES = []


# The files are read and written as bytes: a checkout with CRLF line ends (Windows,
# core.autocrlf) keeps them when the sync runs elsewhere (WSL), and a file whose version is
# already right is not written at all, so a build leaves "git status" clean.
def read_file(path: Path) -> str:
    return path.read_bytes().decode("utf-8")


def write_if_changed(path: Path, content: str) -> bool:
    data = content.encode("utf-8")
    if path.exists() and path.read_bytes() == data:
        return False
    path.write_bytes(data)
    return True


def update_file(path: Path, pattern: str, replacement: str, count_max: int = 0):
    if not path.exists():
        print(f"[-] Missing file: {path}")
        FAILURES.append(str(path))
        return False
    content = read_file(path)
    new_content, count = re.subn(pattern, replacement, content, count=count_max)
    if count == 0:
        print(f"[-] Pattern '{pattern}' not matched in {path}")
        FAILURES.append(str(path))
        return False
    if write_if_changed(path, new_content):
        print(f"[+] Updated {path.relative_to(REPO_ROOT)} ({count} match)")
    else:
        print(f"[=] {path.relative_to(REPO_ROOT)} already up to date")
    return True

def sync_all(new_version, increment_vc=False):
    print(f"[*] Synchronizing project version to {new_version}...")

    # 1. Root VERSION (its line end kept)
    old = read_file(VERSION_FILE) if VERSION_FILE.exists() else ""
    eol = "\r\n" if old.endswith("\r\n") else "\n"
    if write_if_changed(VERSION_FILE, new_version.strip() + eol):
        print(f"[+] Updated VERSION -> {new_version}")
    else:
        print("[=] VERSION already up to date")

    # 2. harmony_version.h
    update_file(
        HARMONY_VERSION_H,
        r'#define\s+HARMONY_VERSION\s+"[^"]+"',
        f'#define HARMONY_VERSION "{new_version}"'
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

    # 4b. package-lock.json: the root package (top level and packages[""]), not dependencies
    update_file(
        PACKAGE_LOCK,
        r'\A(\{\s*"name"\s*:\s*"[^"]*",\s*"version"\s*:\s*)"[^"]+"',
        rf'\g<1>"{new_version}"'
    )
    update_file(
        PACKAGE_LOCK,
        r'("packages"\s*:\s*\{\s*""\s*:\s*\{\s*"name"\s*:\s*"[^"]*",\s*"version"\s*:\s*)"[^"]+"',
        rf'\g<1>"{new_version}"'
    )

    # 5. OpenAPI spec (info.version; tests/test_openapi.py checks it against the firmware version)
    update_file(
        OPENAPI_YAML,
        r'(?m)^(  version: )\S+(?=\r?$)',
        rf'\g<1>{new_version}'
    )

    # 6. build.gradle (versionName & optional versionCode increment)
    if not BUILD_GRADLE.exists():
        print(f"[-] Missing file: {BUILD_GRADLE}")
        FAILURES.append(str(BUILD_GRADLE))
    else:
        content = read_file(BUILD_GRADLE)
        vc_match = re.search(r'versionCode\s+(\d+)', content)
        new_vc = 1
        if vc_match:
            cur_vc = int(vc_match.group(1))
            new_vc = cur_vc + 1 if increment_vc else cur_vc
        content = re.sub(r'versionCode\s+\d+', f'versionCode {new_vc}', content)
        content = re.sub(r'versionName\s+"[^"]+"', f'versionName "{new_version}"', content)
        if write_if_changed(BUILD_GRADLE, content):
            print(f"[+] Updated {BUILD_GRADLE.relative_to(REPO_ROOT)} (versionName='{new_version}', versionCode={new_vc})")
        else:
            print(f"[=] {BUILD_GRADLE.relative_to(REPO_ROOT)} already up to date")

    if FAILURES:
        print(f"[-] Version NOT updated in: {', '.join(FAILURES)}")
        sys.exit(1)
    print(f"[+] Successfully bumped and synced all components to v{new_version}!")

def main():
    parser = argparse.ArgumentParser(description="Synchronize version across Harmony project")
    parser.add_argument("--version", "-v", help="Explicit version string (e.g. 1.3.0)")
    parser.add_argument("--bump", "-b", choices=["patch", "minor", "major"], help="Bump semver part")
    parser.add_argument("--sync", "-s", action="store_true", help="Sync from current VERSION file")
    parser.add_argument("--next", choices=["patch", "minor", "major"],
                        help="Print the next version and change nothing (for the release workflow)")
    args = parser.parse_args()

    if args.next:
        print(bump_semver(get_current_version(), args.next))
        return

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
