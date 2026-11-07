#!/usr/bin/env python3
"""Build Web UI static assets and gzip compress for embedded HTTP delivery.

Usage:
    python build/build_webui.py [--ui legacy|new]
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys


def build_webui(ui: str = "legacy", repo_root: pathlib.Path | None = None) -> bool:
    if repo_root is None:
        repo_root = pathlib.Path(__file__).resolve().parent.parent

    is_legacy = ui in ("legacy", "old", "android-app")
    app_dir = repo_root / ("android-app" if is_legacy else "frontend")
    ui_label = "Legacy WebUI (android-app)" if is_legacy else "New Modular WebUI v3 (frontend)"
    www_dir = repo_root / "payload" / "www"
    assets_dir = www_dir / "assets"
    dist_dir = app_dir / "dist"

    print("============================================================")
    print("  Building Web UI Static Assets (Vite & Gzip Compression)   ")
    print(f"  Selected UI: {ui_label}")
    print("============================================================")

    if not app_dir.is_dir():
        print(f"[-] Error: WebUI directory {app_dir} not found.", file=sys.stderr)
        return False

    # Optional: build wasm / themes if emcc available
    remote_render_script = repo_root / "tools" / "remote_render" / "build.sh"
    theme_builder = repo_root / "tools" / "remote_render" / "build_theme.py"
    if shutil.which("emcc") and remote_render_script.is_file():
        wasm_dest = app_dir / "public" / "assets"
        wasm_dest.mkdir(parents=True, exist_ok=True)
        print("[*] Compiling remote screen renderer WebAssembly...")
        subprocess.run(["bash", str(remote_render_script), "--wasm", str(wasm_dest)], capture_output=True)
        if theme_builder.is_file():
            subprocess.run([sys.executable, str(theme_builder), "--tar", str(wasm_dest / "themes")], capture_output=True)

    # 1. Run Vite build
    print(f"[*] Compiling frontend bundle with Vite in {app_dir.name}...")
    npm_cmd = "npm.cmd" if sys.platform == "win32" else "npm"
    res = subprocess.run([npm_cmd, "run", "build"], cwd=app_dir)
    if res.returncode != 0:
        print(f"[-] Vite build failed with exit code {res.returncode}", file=sys.stderr)
        return False

    if not (dist_dir / "index.html").is_file():
        print(f"[-] Expected build output {dist_dir / 'index.html'} not found.", file=sys.stderr)
        return False

    # 2. Sync to payload/www
    print(f"[*] Syncing compiled assets to {www_dir}...")
    www_dir.mkdir(parents=True, exist_ok=True)
    for p in www_dir.iterdir():
        if p.is_file():
            p.unlink(missing_ok=True)
    if assets_dir.is_dir():
        for p in assets_dir.iterdir():
            if p.is_file():
                p.unlink(missing_ok=True)

    for item in dist_dir.iterdir():
        dest = www_dir / item.name
        if item.is_dir():
            shutil.copytree(item, dest, dirs_exist_ok=True)
        else:
            shutil.copy2(item, dest)

    fav = app_dir / "favicon.png"
    if fav.is_file():
        shutil.copy2(fav, www_dir / "favicon.png")

    # Flash safety: Icons live in /cache/icons.tar.gz and are unpacked to /tmp/www on Hub.
    # Never copy individual icon files into payload/www flash partition.
    payload_icons = assets_dir / "icons"
    if payload_icons.is_dir():
        shutil.rmtree(payload_icons, ignore_errors=True)

    # 3. Gzip compress assets for embedded HTTP server
    print("[*] Gzipping assets (.gz) for fast embedded delivery...")
    gzip_script = repo_root / "build" / "gzip_assets.py"
    if gzip_script.is_file():
        subprocess.run([sys.executable, str(gzip_script), str(www_dir)], capture_output=True)

    print(f"[+] {ui_label} assets built and compressed successfully in payload/www!")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Build and package WebUI static assets")
    parser.add_argument(
        "--ui",
        choices=["legacy", "new", "v3", "frontend", "old", "android-app"],
        default="legacy",
        help="Choice of Web UI to build (default: legacy)",
    )
    args = parser.parse_args()
    success = build_webui(ui=args.ui)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
