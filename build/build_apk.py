#!/usr/bin/env python3
"""Build Harmony Remote Android APK.

Usage:
    python build/build_apk.py [--release]
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys


def build_apk(release: bool = False, repo_root: pathlib.Path | None = None) -> bool:
    if repo_root is None:
        repo_root = pathlib.Path(__file__).resolve().parent.parent

    android_app_dir = repo_root / "android-app"
    android_dir = android_app_dir / "android"
    output_dir = repo_root / "build" / "output" / "apk"

    if not android_app_dir.is_dir():
        print(f"[-] Error: android-app directory not found at {android_app_dir}", file=sys.stderr)
        return False

    print("============================================================")
    print("  Building Harmony Remote Android APK")
    print(f"  Configuration: {'Release' if release else 'Debug'}")
    print("============================================================")

    # Windows: check Android Studio JBR if JAVA_HOME not already set
    if sys.platform == "win32" and "JAVA_HOME" not in os.environ:
        studio_jbr = pathlib.Path(r"C:\Program Files\Android\Android Studio\jbr")
        if studio_jbr.is_dir():
            os.environ["JAVA_HOME"] = str(studio_jbr)
            os.environ["PATH"] = f"{studio_jbr / 'bin'};{os.environ.get('PATH', '')}"

    npm_cmd = "npm.cmd" if sys.platform == "win32" else "npm"
    npx_cmd = "npx.cmd" if sys.platform == "win32" else "npx"

    # 1. Build Vite web bundle
    print("[1/4] Building Vite web bundle...")
    res = subprocess.run([npm_cmd, "run", "build"], cwd=android_app_dir)
    if res.returncode != 0:
        print(f"[-] Vite build failed with exit code {res.returncode}", file=sys.stderr)
        return False

    # Sync web assets to payload/www
    print("[+] Syncing web assets to payload/www...")
    www_dir = repo_root / "payload" / "www"
    www_dir.mkdir(parents=True, exist_ok=True)
    dist_dir = android_app_dir / "dist"
    for item in dist_dir.iterdir():
        dest = www_dir / item.name
        if item.is_dir():
            shutil.copytree(item, dest, dirs_exist_ok=True)
        else:
            shutil.copy2(item, dest)

    gzip_script = repo_root / "build" / "gzip_assets.py"
    if gzip_script.is_file():
        subprocess.run([sys.executable, str(gzip_script), str(www_dir)], capture_output=True)

    # 2. Capacitor sync
    print("[2/4] Running Capacitor sync...")
    res = subprocess.run([npx_cmd, "cap", "sync", "android"], cwd=android_app_dir)
    if res.returncode != 0:
        print(f"[-] Capacitor sync failed with exit code {res.returncode}", file=sys.stderr)
        return False

    # 3. Gradle build
    print("[3/4] Running Gradle assemble...")
    gradle_task = "assembleRelease" if release else "assembleDebug"
    gradle_bin = android_dir / ("gradlew.bat" if sys.platform == "win32" else "gradlew")
    if sys.platform != "win32":
        try:
            gradle_bin.chmod(0o755)
        except Exception:
            pass

    cmd = [str(gradle_bin), gradle_task]
    res = subprocess.run(cmd, cwd=android_dir, shell=(sys.platform == "win32"))
    if res.returncode != 0:
        print(f"[-] Gradle build failed with exit code {res.returncode}", file=sys.stderr)
        return False

    # 4. Collect APKs
    print("[4/4] Collecting generated APKs...")
    output_dir.mkdir(parents=True, exist_ok=True)
    apk_pattern = "*-release*.apk" if release else "*-debug*.apk"
    apk_source_dir = android_dir / "app" / "build" / "outputs" / "apk" / ("release" if release else "debug")

    collected = 0
    if apk_source_dir.is_dir():
        for apk in apk_source_dir.glob("*.apk"):
            dest_apk = output_dir / apk.name
            shutil.copy2(apk, dest_apk)
            sz_mb = apk.stat().st_size / (1024 * 1024)
            print(f"[+] Output: {dest_apk} ({sz_mb:.2f} MB)")
            collected += 1

    if collected == 0:
        print("[-] Warning: No APKs found in build output directory.", file=sys.stderr)
        return False

    print("[+] Android APK build completed successfully!")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Build Harmony Remote Android APK")
    parser.add_argument("--release", action="store_true", help="Build release APK instead of debug APK")
    args = parser.parse_args()
    success = build_apk(release=args.release)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
