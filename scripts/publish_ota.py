#!/usr/bin/env python3
"""
publish_ota.py - Package and publish binaries & WebUI assets to OTA repository / branch

Features:
  1. Packages native MIPS binaries (Hub), ARM binary (Elite), and WebUI bundle (www.tar.gz)
  2. Generates manifest.json with SHA256 and MD5 checksums
  3. Prepares or pushes to dedicated orphan branch 'ota' (or remote repository) with a single squashed commit
"""

import os
import sys
import json
import hashlib
import tarfile
import argparse
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIPS_SEARCH_PATHS = [
    REPO_ROOT / "artifacts" / "hub-binaries",
    REPO_ROOT / "build" / "output" / "hub",
    REPO_ROOT / "build" / "output",
    REPO_ROOT / "artifacts" / "mips-binaries",
    REPO_ROOT / "payload" / "bin",
]
ARM_SEARCH_PATHS = [
    REPO_ROOT / "artifacts" / "elite-binaries",
    REPO_ROOT / "build" / "output" / "elite",
    REPO_ROOT / "build" / "output",
    REPO_ROOT / "elite" / "bin",
]
WWW_SEARCH_PATHS = [
    REPO_ROOT / "artifacts" / "webui-assets",
    REPO_ROOT / "payload" / "www",
    REPO_ROOT / "android-app" / "dist",
]
VERSION_FILE = REPO_ROOT / "VERSION"
STAGING_DIR = REPO_ROOT / "build" / "ota_staging"

HUB_BINARIES = [
    "codex_daemon",
    "codex_rf",
    "codex_btstack",
    "codex_dhcpd",
    "codex_portal",
    "codex_sntp",
    "codex_sync",
    "mknod",
    "register_ehci",
]

ELITE_BINARIES = [
    "codex_elite",
    "codex_elite.ota.gz",
    "codex_sync",
]

def find_candidate_file(name: str, search_paths: list[Path]) -> Path | None:
    for sp in search_paths:
        candidate = sp / name
        if candidate.is_file():
            return candidate
    return None

def get_file_hashes(path: Path):
    h_md5 = hashlib.md5()
    h_sha = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h_md5.update(chunk)
            h_sha.update(chunk)
    return h_md5.hexdigest().lower(), h_sha.hexdigest().lower()

def package_ota(output_dir: Path, version: str):
    print(f"[*] Packaging OTA release v{version} to {output_dir}...")
    if output_dir.exists():
        import shutil
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # 1. Version file
    (output_dir / "VERSION").write_text(version + "\n", encoding="utf-8")

    # 2. MIPS binaries
    mips_target = output_dir / "bin" / "mips"
    mips_target.mkdir(parents=True, exist_ok=True)
    manifest_files = {}

    for bname in HUB_BINARIES:
        src = find_candidate_file(bname, MIPS_SEARCH_PATHS)
        if src and src.exists():
            dst = mips_target / bname
            import shutil
            shutil.copy2(src, dst)
            md5, sha = get_file_hashes(dst)
            manifest_files[f"bin/mips/{bname}"] = {
                "size": dst.stat().st_size,
                "md5": md5,
                "sha256": sha
            }
        else:
            print(f"[-] Warning: Hub binary {bname} not found in search paths")

    # 3. ARM binaries (Elite)
    arm_target = output_dir / "bin" / "arm"
    arm_target.mkdir(parents=True, exist_ok=True)
    for bname in ELITE_BINARIES:
        src = find_candidate_file(bname, ARM_SEARCH_PATHS)
        if (not src or not src.exists()) and bname == "codex_elite.ota.gz":
            src = find_candidate_file("codex_elite.gz", ARM_SEARCH_PATHS)
        if src and src.exists():
            dst = arm_target / bname
            import shutil
            shutil.copy2(src, dst)
            md5, sha = get_file_hashes(dst)
            manifest_files[f"bin/arm/{bname}"] = {
                "size": dst.stat().st_size,
                "md5": md5,
                "sha256": sha
            }
        else:
            print(f"[-] Warning: Elite binary {bname} not found in search paths")

    # 4. WebUI bundle (tar.gz)
    www_src = None
    for wsp in WWW_SEARCH_PATHS:
        if (wsp / "index.html").is_file():
            www_src = wsp
            break

    www_tar = output_dir / "www.tar.gz"
    if www_src:
        print(f"[*] Creating {www_tar.name} from {www_src}...")
        with tarfile.open(www_tar, "w:gz") as tar:
            for root, _, files in os.walk(www_src):
                for file in files:
                    full_path = Path(root) / file
                    rel_path = full_path.relative_to(www_src)
                    tar.add(full_path, arcname=str(rel_path).replace("\\", "/"))

        md5, sha = get_file_hashes(www_tar)
        manifest_files["www.tar.gz"] = {
            "size": www_tar.stat().st_size,
            "md5": md5,
            "sha256": sha
        }
    else:
        print("[-] Warning: No built WebUI bundle found with index.html")

    # 5. Manifest.json
    manifest = {
        "version": version,
        "files": manifest_files
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"[+] Packaged {len(manifest_files)} artifacts into {output_dir}")

def push_to_ota_branch(staging_dir: Path, version: str, remote_url: str = None, branch: str = "ota"):
    print(f"[*] Preparing git commit on single squashed branch '{branch}'...")
    import tempfile
    import shutil

    with tempfile.TemporaryDirectory() as tmp_git:
        tmp_path = Path(tmp_git)
        # Initialize repository on target branch
        subprocess.run(["git", "init", "-b", branch], cwd=tmp_path, check=True)
        # Configure author identity for newly initialized repo
        subprocess.run(["git", "config", "user.name", "github-actions[bot]"], cwd=tmp_path, check=True)
        subprocess.run(["git", "config", "user.email", "github-actions[bot]@users.noreply.github.com"], cwd=tmp_path, check=True)
        # Copy staging contents
        for item in staging_dir.iterdir():
            if item.is_dir():
                shutil.copytree(item, tmp_path / item.name)
            else:
                shutil.copy2(item, tmp_path / item.name)

        subprocess.run(["git", "add", "."], cwd=tmp_path, check=True)
        subprocess.run(["git", "commit", "-m", f"OTA Release v{version} (single squashed commit)"], cwd=tmp_path, check=True)

        if remote_url:
            print(f"[*] Force-pushing to {remote_url} branch {branch}...")
            subprocess.run(["git", "remote", "add", "origin", remote_url], cwd=tmp_path, check=True)
            subprocess.run(["git", "push", "-f", "origin", branch], cwd=tmp_path, check=True)
            print(f"[+] Successfully pushed single-commit OTA release to {remote_url}:{branch}!")
        else:
            print(f"[+] Single-commit git repository created locally in staging for branch '{branch}'.")
            print("    To push to GitHub, run with --remote <url> or git push -f.")

def main():
    parser = argparse.ArgumentParser(description="Package and deploy Harmony Codex OTA assets")
    parser.add_argument("--version", "-v", help="Release version (default: from VERSION file)")
    parser.add_argument("--push", "-p", action="store_true", help="Push to git remote OTA branch")
    parser.add_argument("--remote", "-r", default="", help="Remote git URL (e.g. git@github.com:iranl/harmony-ng.git)")
    parser.add_argument("--branch", "-b", default="ota", help="Target branch (default: ota)")
    args = parser.parse_args()

    ver = args.version or (VERSION_FILE.read_text().strip() if VERSION_FILE.exists() else "1.3.0")
    package_ota(STAGING_DIR, ver)

    if args.push or args.remote:
        push_to_ota_branch(STAGING_DIR, ver, remote_url=args.remote or None, branch=args.branch)

if __name__ == "__main__":
    main()
