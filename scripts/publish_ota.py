#!/usr/bin/env python3
"""
publish_ota.py - Package and publish binaries & WebUI assets to OTA repository / branch

Features:
  1. Packages native MIPS binaries (Hub), ARM binary (remote), and WebUI bundle (www.tar.gz)
  2. Generates manifest.json with SHA256 and MD5 checksums
  3. Prepares or pushes to dedicated orphan branch 'ota' (or remote repository) with a single squashed commit
"""

import os
import sys
import json
import hashlib
import shutil
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
    REPO_ROOT / "artifacts" / "remote-binaries",
    REPO_ROOT / "build" / "output" / "remote",
    REPO_ROOT / "build" / "output",
    REPO_ROOT / "remote" / "bin",
]
WWW_SEARCH_PATHS = [
    REPO_ROOT / "artifacts" / "webui-assets",
    REPO_ROOT / "payload" / "www",
    REPO_ROOT / "android-app" / "dist",
]
VERSION_FILE = REPO_ROOT / "VERSION"
STAGING_DIR = REPO_ROOT / "build" / "ota_staging"

HUB_BINARIES = [
    "hub_util",
    "hub_daemon",
    "hub_rf",
    "hub_btstack",
    "hub_dhcpd",
    "hub_portal",
    "hub_sntp",
    "hub_sync",
    "mknod",
    "register_ehci",
    "eth_fix",
]

REMOTE_BINARIES = [
    "remote_ui",
    "remote_ui.ota.gz",
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
    # For v2+ releases, keep root VERSION at 1.14.0 so existing 1.*.* hubs do not attempt 2.0 upgrade over the air.
    # Write v2/VERSION and VERSION_V2 for 2.0+ clients.
    try:
        major = int(version.split(".")[0])
    except Exception:
        major = 1

    if major >= 2:
        (output_dir / "VERSION").write_text("1.14.0\n", encoding="utf-8")
        (output_dir / "VERSION_V2").write_text(version + "\n", encoding="utf-8")
        v2_dir = output_dir / "v2"
        v2_dir.mkdir(parents=True, exist_ok=True)
        (v2_dir / "VERSION").write_text(version + "\n", encoding="utf-8")
    else:
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

    # 3. ARM binaries (remote)
    arm_target = output_dir / "bin" / "arm"
    arm_target.mkdir(parents=True, exist_ok=True)
    for bname in REMOTE_BINARIES:
        if bname == "remote_ui.ota.gz":
            remote_bin = find_candidate_file("remote_ui", ARM_SEARCH_PATHS)
            if remote_bin and remote_bin.is_file():
                ota_tar = arm_target / bname
                print(f"[*] Packaging remote OTA bundle (binary + scripts) into {ota_tar.name}...")
                with tarfile.open(ota_tar, "w:gz") as tar:
                    tar.add(remote_bin, arcname="remote_ui")
                    # Transition: a remote on a build from before the elite -> remote rename looks
                    # for "elite_ui" in the package; the new binary migrates its files at start
                    tar.add(remote_bin, arcname="elite_ui")
                    for sf in ["rcS.local", "init.sh", "remote_watchdog.sh", "safe_reboot.sh", "factory_reset.sh"]:
                        sp = REPO_ROOT / "remote" / sf
                        if sp.is_file():
                            tar.add(sp, arcname=sf)
                md5, sha = get_file_hashes(ota_tar)
                manifest_files[f"bin/arm/{bname}"] = {
                    "size": ota_tar.stat().st_size,
                    "md5": md5,
                    "sha256": sha
                }
                # Hubs on a build from before the rename download bin/arm/elite_ui.ota.gz
                legacy = arm_target / "elite_ui.ota.gz"
                shutil.copyfile(ota_tar, legacy)
                manifest_files["bin/arm/elite_ui.ota.gz"] = dict(manifest_files[f"bin/arm/{bname}"])
                continue

        src = find_candidate_file(bname, ARM_SEARCH_PATHS)
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
            print(f"[-] Warning: remote binary {bname} not found in search paths")

    # 4. WebUI bundle (tar.gz)
    www_src = None
    for wsp in WWW_SEARCH_PATHS:
        if (wsp / "index.html").is_file() or (wsp / "index.html.gz").is_file():
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

    # 5. Unified Hub OTA bundle (hub_ota.tar.gz)
    hub_tar = output_dir / "hub_ota.tar.gz"
    print(f"[*] Creating unified Hub OTA package {hub_tar.name}...")
    with tarfile.open(hub_tar, "w:gz") as tar:
        # Version
        ver_file = (output_dir / "v2" / "VERSION") if (major >= 2 and (output_dir / "v2" / "VERSION").exists()) else (output_dir / "VERSION")
        tar.add(ver_file, arcname="VERSION")
        # Binaries
        for bname in HUB_BINARIES + ["busybox", "dropbearmulti"]:
            src = find_candidate_file(bname, MIPS_SEARCH_PATHS)
            if src and src.exists():
                tar.add(src, arcname=f"bin/{bname}")
        # Scripts
        scripts_dir = REPO_ROOT / "payload" / "scripts"
        if scripts_dir.is_dir():
            for f in scripts_dir.iterdir():
                if f.is_file():
                    tar.add(f, arcname=f"scripts/{f.name}")
        # Config (CA bundle etc.)
        config_dir = REPO_ROOT / "payload" / "config"
        if config_dir.is_dir():
            for f in config_dir.iterdir():
                if f.is_file():
                    tar.add(f, arcname=f"config/{f.name}")
        # WebUI
        if www_src and www_src.is_dir():
            for root, _, files in os.walk(www_src):
                for file in files:
                    full_path = Path(root) / file
                    rel_path = full_path.relative_to(www_src)
                    tar.add(full_path, arcname=f"www/{str(rel_path).replace(os.sep, '/')}")

        # Icons Archive
        icons_tar = REPO_ROOT / "build" / "icons.tar.gz"
        if not icons_tar.is_file() and (REPO_ROOT / "data" / "remote_icons" / "icons").is_dir():
            print("[*] Creating icons.tar.gz from data/remote_icons/icons...")
            with tarfile.open(icons_tar, "w:gz") as itar:
                itar.add(REPO_ROOT / "data" / "remote_icons" / "icons", arcname="icons")
        if icons_tar.is_file():
            tar.add(icons_tar, arcname="icons.tar.gz")
            shutil.copy2(icons_tar, output_dir / "icons.tar.gz")
            imd5, isha = get_file_hashes(output_dir / "icons.tar.gz")
            manifest_files["icons.tar.gz"] = {
                "size": (output_dir / "icons.tar.gz").stat().st_size,
                "md5": imd5,
                "sha256": isha
            }

    md5, sha = get_file_hashes(hub_tar)
    manifest_files["hub_ota.tar.gz"] = {
        "size": hub_tar.stat().st_size,
        "md5": md5,
        "sha256": sha
    }

    # MD5 checksum files expected by Hub OTA updater
    (output_dir / "hub_ota.tar.gz.md5").write_text(f"{md5}  hub_ota.tar.gz\n", encoding="utf-8")
    (output_dir / "hub_ota.md5").write_text(f"{md5}\n", encoding="utf-8")
    if (output_dir / "v2").is_dir():
        (output_dir / "v2" / "hub_ota.tar.gz.md5").write_text(f"{md5}  hub_ota.tar.gz\n", encoding="utf-8")
        (output_dir / "v2" / "hub_ota.md5").write_text(f"{md5}\n", encoding="utf-8")

    # 6. Manifest.json
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
    parser = argparse.ArgumentParser(description="Package and deploy Harmony OTA assets")
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
