#!/usr/bin/env python3
"""
Extract icons from Logitech Harmony Elite remote and push directly to Hub WebUI.

Usage:
    python tools/extract_remote_icons.py [--remote 192.168.2.1] [--hub <HUB_IP>] [--used-only]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

DEFAULT_REMOTE_IP = "192.168.2.1"
DEFAULT_HUB_IP = os.environ.get("HUB_IP", "192.168.1.1")
REMOTE_ASSET_BASE = "/opt/luadraws/appscommon/asset"


def run_ssh(host, cmd, binary=False):
    """Run command on remote via ssh."""
    args = [
        "ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
        f"root@{host}", cmd
    ]
    res = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if res.returncode != 0:
        return None
    return res.stdout if binary else res.stdout.decode("utf-8", errors="replace")


def scp_pull(host, remote_path, local_path):
    """Pull file from remote host using scp with legacy protocol (-O)."""
    os.makedirs(os.path.dirname(local_path), exist_ok=True)
    args = [
        "scp", "-O", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
        "-q", f"root@{host}:{remote_path}", local_path
    ]
    res = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return res.returncode == 0


def scp_push(host, local_path, remote_path):
    """Push file to remote host using scp with legacy protocol (-O)."""
    args = [
        "scp", "-O", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
        "-q", local_path, f"root@{host}:{remote_path}"
    ]
    res = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return res.returncode == 0


def list_remote_icons(remote_ip):
    """Find all PNG files in asset directory on Elite remote."""
    print(f"[*] Listing asset files on Elite remote ({remote_ip})...")
    # Busybox find might not support -name, use ls across subdirs
    cmd = (
        f"for d in footer commands settings gesture hue juniperactivity hopsactivity "
        f"juniperDevice hopsDevice juniperHomeAutomation header notification home; do "
        f"  ls {REMOTE_ASSET_BASE}/$d/*.png 2>/dev/null; "
        f"done"
    )
    out = run_ssh(remote_ip, cmd)
    if not out:
        # Fallback to single ls
        out = run_ssh(remote_ip, f"ls {REMOTE_ASSET_BASE}/*/*.png 2>/dev/null")
    if not out:
        return []
    lines = [line.strip() for line in out.splitlines() if line.strip().endswith(".png")]
    return sorted(list(set(lines)))


def find_used_icon_patterns(repo_root):
    """Scan local config files for icon references."""
    used = set()
    cfg_paths = [
        os.path.join(repo_root, "data", "resources", "ActivityList.json"),
        os.path.join(repo_root, "data", "resources", "DeviceList.json"),
        os.path.join(repo_root, "payload", "source", "elite", "elite_main.c")
    ]
    for p in cfg_paths:
        if not os.path.isfile(p):
            continue
        try:
            with open(p, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
            # Find UUIDs or named PNGs
            matches = re.findall(r'([A-Za-z0-9_\-]+\.png)', content)
            for m in matches:
                used.add(m.lower())
                # Also strip _small, _smallon, etc.
                base = re.sub(r'_(small|smallon|on|off)$', '', m.lower().replace('.png', ''))
                used.add(base)
        except Exception:
            pass
    return used


def main():
    parser = argparse.ArgumentParser(description="Extract Harmony Elite icons and push to Hub.")
    parser.add_argument("--remote", default=DEFAULT_REMOTE_IP, help=f"Elite remote IP (default: {DEFAULT_REMOTE_IP})")
    parser.add_argument("--hub", default=DEFAULT_HUB_IP, help=f"Harmony Hub IP (default: {DEFAULT_HUB_IP}, or '' to skip push)")
    parser.add_argument("--output", default="", help="Local destination directory for extracted icons")
    parser.add_argument("--used-only", action="store_true", help="Only extract icons referenced in config files")
    parser.add_argument("--skip-push", action="store_true", help="Do not push icons to Hub")
    args = parser.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    out_dir = args.output or os.path.join(repo_root, "payload", "www", "assets", "icons")
    os.makedirs(out_dir, exist_ok=True)

    print("==================================================")
    print("  Harmony Elite -> Hub Icon Sync Utility")
    print("==================================================")
    print(f"Remote (Source): {args.remote}")
    print(f"Local Output:    {out_dir}")
    if args.hub and not args.skip_push:
        print(f"Hub (Target):    {args.hub}")
    print("--------------------------------------------------")

    # 1. Test connection to remote
    test = run_ssh(args.remote, "echo ok")
    if not test or "ok" not in test:
        print(f"[!] Error: Unable to connect to Elite remote at {args.remote} via SSH.")
        print("    Ensure Elite is connected via USB and USB-Ethernet is active.")
        sys.exit(1)

    # 2. Get list of icons
    remote_files = list_remote_icons(args.remote)
    if not remote_files:
        print("[!] No PNG icon assets found on remote.")
        sys.exit(1)

    print(f"[+] Found {len(remote_files)} total icon assets on remote.")

    filter_used = args.used_only
    used_patterns = set()
    if filter_used:
        used_patterns = find_used_icon_patterns(repo_root)
        print(f"[*] Filtering by {len(used_patterns)} detected icon keys in config...")

    files_to_pull = []
    for rf in remote_files:
        fn = os.path.basename(rf).lower()
        if not filter_used:
            files_to_pull.append(rf)
            continue
        # Check if matched in used patterns
        base = re.sub(r'_(small|smallon|on|off)\.png$', '.png', fn)
        base_no_ext = base.replace('.png', '')
        if fn in used_patterns or base in used_patterns or base_no_ext in used_patterns:
            files_to_pull.append(rf)

    if not files_to_pull:
        print("[!] No icons matched filter criteria. Pulling all core UI icons instead.")
        files_to_pull = [rf for rf in remote_files if any(k in rf for k in ["footer", "hopsactivity", "juniperactivity", "commands"])]

    print(f"[*] Extracting {len(files_to_pull)} icons to local cache...")
    success_count = 0
    extracted_rel_paths = []

    for rf in files_to_pull:
        rel = rf.replace(f"{REMOTE_ASSET_BASE}/", "")
        loc_path = os.path.join(out_dir, rel)
        if scp_pull(args.remote, rf, loc_path):
            success_count += 1
            extracted_rel_paths.append((loc_path, rel))
            # Also create flat copy for direct WebUI name resolution
            flat_name = os.path.basename(rf)
            flat_path = os.path.join(out_dir, flat_name)
            if flat_path != loc_path and not os.path.exists(flat_path):
                try:
                    with open(loc_path, "rb") as sf, open(flat_path, "wb") as df:
                        df.write(sf.read())
                except Exception:
                    pass

    print(f"[+] Successfully extracted {success_count} icons to {out_dir}")

    # 3. Push to Hub if configured
    if args.hub and not args.skip_push:
        print(f"\n[*] Pushing icon pack directly to Hub ({args.hub})...")
        # Check connection
        hub_ok = run_ssh(args.hub, "echo ok")
        if not hub_ok or "ok" not in hub_ok:
            print(f"[!] Warning: Cannot reach Hub at {args.hub}. Skipping direct push.")
            print("    Run deploy_hub.sh later to deploy icons.")
            return

        # Create single zip bundle for atomic high-speed transfer
        with tempfile.NamedTemporaryFile(suffix=".zip", delete=False) as tmp_zip:
            tmp_zip_path = tmp_zip.name

        try:
            with zipfile.ZipFile(tmp_zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
                for root, _, files in os.walk(out_dir):
                    for file in files:
                        full_p = os.path.join(root, file)
                        arc_name = os.path.relpath(full_p, out_dir).replace("\\", "/")
                        zf.write(full_p, arc_name)

            print(f"[*] Created icon bundle ({os.path.getsize(tmp_zip_path) // 1024} KB). Uploading...")
            if not scp_push(args.hub, tmp_zip_path, "/tmp/icons.zip"):
                print("[!] Failed to upload icon bundle to Hub.")
                return

            # Extract on Hub and flatten
            extract_cmd = (
                "mkdir -p /cache/www/assets/icons /data/harmony/www/assets && "
                "[ -L /data/harmony/www/assets/icons ] || ln -sfn /cache/www/assets/icons /data/harmony/www/assets/icons; "
                "cd /cache/www/assets/icons && "
                "unzip -o /tmp/icons.zip && "
                "rm -f /tmp/icons.zip; "
                "for f in */*.png; do [ -f \"$f\" ] || continue; base=\"${f##*/}\"; [ ! -e \"$base\" ] && ln -sf \"$f\" \"$base\" 2>/dev/null || true; done; "
                "[ -e juniperDevice/1.png ] && [ ! -e tv.png ] && ln -sf juniperDevice/1.png tv.png 2>/dev/null; "
                "[ -e notification/tv.png ] && ln -sf notification/tv.png tv.png 2>/dev/null; "
                "[ -e juniperDevice/5.png ] && [ ! -e receiver.png ] && ln -sf juniperDevice/5.png receiver.png 2>/dev/null; "
                "[ -e juniperDevice/default.png ] && [ ! -e generic.png ] && ln -sf juniperDevice/default.png generic.png 2>/dev/null; "
                "[ -e commands/116.png ] && [ ! -e poweroff.png ] && ln -sf commands/116.png poweroff.png 2>/dev/null; "
                "[ -e notification/switch.png ] && [ ! -e switch.png ] && ln -sf notification/switch.png switch.png 2>/dev/null; "
                "[ -e juniperHomeAutomation/bulb_on.png ] && [ ! -e light.png ] && ln -sf juniperHomeAutomation/bulb_on.png light.png 2>/dev/null"
            )
            run_ssh(args.hub, extract_cmd)
            count_hub = run_ssh(args.hub, "ls /cache/www/assets/icons/*.png 2>/dev/null | wc -l")
            cnt_str = count_hub.strip() if count_hub else str(success_count)
            print(f"[+] Successfully deployed {cnt_str} icons on Hub (/data/harmony/www/assets/icons/)!")
        finally:
            if os.path.exists(tmp_zip_path):
                os.remove(tmp_zip_path)

    print("\n[+] Icon extraction and deployment complete!")


if __name__ == "__main__":
    main()
