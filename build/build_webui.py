#!/usr/bin/env python3
"""Build Web UI static assets and gzip compress for embedded HTTP delivery.

Usage:
    python build/build_webui.py [--ui legacy|new]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys


def npm_command() -> str:
    return "npm.cmd" if sys.platform == "win32" else "npm"


LOCK_STAMP = ".harmony-lock.sha256"  # sha256 of the package-lock.json node_modules was installed from


def _lock_digest(lock: pathlib.Path) -> str:
    return hashlib.sha256(lock.read_bytes()).hexdigest()


def ensure_node_modules(app_dir: pathlib.Path) -> bool:
    """Install the web UI's packages with `npm ci` when they are missing or were installed from
    another package-lock.json (a fresh clone or worktree has none, and `npm run build` then stops
    at "vite: not found"). The lock file's content is compared, not its date: a checkout or rebase
    that rewrites the same lock file installs nothing (`npm ci` deletes node_modules first, needs
    the network, and on Windows can fail while a dev server holds files there). False when the
    install failed."""
    lock = app_dir / "package-lock.json"
    modules = app_dir / "node_modules"
    marker = modules / ".package-lock.json"  # written by npm at every install
    stamp = modules / LOCK_STAMP
    if marker.is_file():
        if not lock.is_file():
            return True
        digest = _lock_digest(lock)
        try:
            recorded = stamp.read_text(encoding="ascii").strip()
        except OSError:
            recorded = ""
        if recorded == digest:
            return True
        if not recorded and marker.stat().st_mtime >= lock.stat().st_mtime:
            # installed before the stamp existed: trust the dates once, then record the content
            try:
                stamp.write_text(digest + "\n", encoding="ascii")
            except OSError:
                pass
            return True
    args = ["ci"] if lock.is_file() else ["install"]
    print(f"[*] Installing web UI packages (npm {args[0]} in {app_dir.name})...")
    res = subprocess.run([npm_command(), *args], cwd=app_dir)
    if res.returncode != 0:
        print(f"[-] npm {args[0]} failed in {app_dir.name} (exit code {res.returncode})", file=sys.stderr)
        return False
    if lock.is_file() and modules.is_dir():
        try:
            stamp.write_text(_lock_digest(lock) + "\n", encoding="ascii")
        except OSError:
            pass
    return True


def build_extras(app_dir: pathlib.Path, repo_root: pathlib.Path) -> bool:
    """When emcc is there, build the remote screen renderer (WebAssembly) and the theme
    packages into <app>/public/assets for the screen preview. False when one of them failed
    (the previous files would otherwise be shipped as if they were new)."""
    remote_render_script = repo_root / "tools" / "remote_render" / "build.sh"
    theme_builder = repo_root / "tools" / "remote_render" / "build_theme.py"
    if not (shutil.which("emcc") and remote_render_script.is_file()):
        return True
    wasm_dest = app_dir / "public" / "assets"
    wasm_dest.mkdir(parents=True, exist_ok=True)
    print("[*] Compiling remote screen renderer WebAssembly...")
    res = subprocess.run(["bash", str(remote_render_script), "--wasm", str(wasm_dest)], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] WebAssembly build failed: {(res.stderr or res.stdout).strip()[-500:]}", file=sys.stderr)
        return False
    if theme_builder.is_file():
        res = subprocess.run([sys.executable, str(theme_builder), "--tar", str(wasm_dest / "themes")],
                             capture_output=True, text=True)
        if res.returncode != 0:
            print(f"[-] Theme build failed: {(res.stderr or res.stdout).strip()[-500:]}", file=sys.stderr)
            return False
    return True


def sync_dist_to_www(dist_dir: pathlib.Path, www_dir: pathlib.Path, repo_root: pathlib.Path,
                     favicon: pathlib.Path | None = None) -> bool:
    """Replace the whole content of www_dir with a Vite build and gzip it, as the Hub serves it.

    Everything in www_dir goes first: old hashed bundles, a theme removed from the project, or
    the other web UI's files would otherwise stay next to the new build. False when the copy
    or the gzip step failed."""
    if not (dist_dir / "index.html").is_file():
        print(f"[-] Expected build output {dist_dir / 'index.html'} not found.", file=sys.stderr)
        return False
    print(f"[*] Syncing compiled assets to {www_dir}...")
    www_dir.mkdir(parents=True, exist_ok=True)
    for p in www_dir.iterdir():
        if p.is_dir() and not p.is_symlink():
            shutil.rmtree(p)
        else:
            p.unlink()
    shutil.copytree(dist_dir, www_dir, dirs_exist_ok=True)
    if favicon and favicon.is_file():
        shutil.copy2(favicon, www_dir / "favicon.png")

    # Flash safety: Icons live in /cache/icons.tar.gz (packed: the Hub reads them from flash; an
    # archive without the pack index is unpacked to /tmp/www, as before).
    # Never copy individual icon files into payload/www flash partition.
    shutil.rmtree(www_dir / "assets" / "icons", ignore_errors=True)

    print("[*] Gzipping assets (.gz) for fast embedded delivery...")
    gzip_script = repo_root / "build" / "gzip_assets.py"
    res = subprocess.run([sys.executable, str(gzip_script), str(www_dir)], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] Gzip step failed: {(res.stderr or res.stdout).strip()[-500:]}", file=sys.stderr)
        return False
    return True


def build_webui(ui: str = "legacy", repo_root: pathlib.Path | None = None) -> bool:
    if repo_root is None:
        repo_root = pathlib.Path(__file__).resolve().parent.parent

    is_legacy = ui in ("legacy", "old", "android-app")
    app_dir = repo_root / ("android-app" if is_legacy else "frontend")
    ui_label = "Legacy WebUI (android-app)" if is_legacy else "New Modular WebUI v3 (frontend)"
    www_dir = repo_root / "payload" / "www"
    dist_dir = app_dir / "dist"

    print("============================================================")
    print("  Building Web UI Static Assets (Vite & Gzip Compression)   ")
    print(f"  Selected UI: {ui_label}")
    print("============================================================")

    if not app_dir.is_dir():
        print(f"[-] Error: WebUI directory {app_dir} not found.", file=sys.stderr)
        return False

    if not build_extras(app_dir, repo_root):
        return False

    if not ensure_node_modules(app_dir):
        return False

    # 1. Run Vite build
    print(f"[*] Compiling frontend bundle with Vite in {app_dir.name}...")
    res = subprocess.run([npm_command(), "run", "build"], cwd=app_dir)
    if res.returncode != 0:
        print(f"[-] Vite build failed with exit code {res.returncode}", file=sys.stderr)
        return False

    # 2. Sync to payload/www and gzip
    if not sync_dist_to_www(dist_dir, www_dir, repo_root, favicon=app_dir / "favicon.png"):
        return False

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
