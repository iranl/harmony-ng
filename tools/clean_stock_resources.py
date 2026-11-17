#!/usr/bin/env python3
"""Empty Logitech Stock Resources (/data/resources) on Harmony Hub.

Logitech stock device, activity, and protocol profiles stored in /data/resources
are unused once configurations are imported into HarmonyNG.
This tool safely removes all files and subdirectories within /data/resources
(leaving the empty directory intact), flushes flash buffers, and reports reclaimed space.

Usage:
    python tools/clean_stock_resources.py <HUB_IP> [--dry-run] [--yes]
    python tools/clean_stock_resources.py 10.234.234.98 --dry-run
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
if (SCRIPT_DIR / "ssh_hostkeys.py").is_file():
    import ssh_hostkeys  # noqa: E402


def find_default_ssh_key() -> str | None:
    env_key = os.environ.get("SSH_KEY")
    if env_key and os.path.isfile(env_key):
        return env_key
    ssh_dir = pathlib.Path.home() / ".ssh"
    if ssh_dir.is_dir():
        owner_keys = sorted(
            [p for p in ssh_dir.glob("harmony_owner_*") if not p.name.endswith(".pub")],
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if owner_keys:
            return str(owner_keys[0])
        for std in ("id_rsa", "id_ed25519"):
            cand = ssh_dir / std
            if cand.is_file():
                return str(cand)
    return None


def get_ssh_opts(key_path: str | None = None) -> list[str]:
    opts = [
        "-o", "BatchMode=yes",
        "-o", "UserKnownHostsFile=" + ("NUL" if sys.platform == "win32" else "/dev/null"),
        "-o", "StrictHostKeyChecking=no",
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=5",
    ]
    key = key_path or find_default_ssh_key()
    if key and os.path.isfile(key):
        opts.extend(["-i", key])
    return opts


def run_ssh_cmd(host: str, user: str, cmd: str, ssh_opts: list[str]) -> subprocess.CompletedProcess[str]:
    target = f"{user}@{host}"
    return subprocess.run(["ssh"] + ssh_opts + [target, cmd], capture_output=True, text=True, timeout=30)


def scan_stock_resources(host: str, user: str, ssh_opts: list[str]) -> tuple[list[tuple[str, int]], int]:
    """Scan /data/resources on the Hub and return list of (relative_filename, size_bytes) and total bytes."""
    cmd = 'if [ -d /data/resources ]; then ls -l /data/resources 2>/dev/null; fi'
    res = run_ssh_cmd(host, user, cmd, ssh_opts)
    if res.returncode != 0:
        raise RuntimeError(f"SSH command failed ({res.returncode}): {res.stderr.strip() or res.stdout.strip()}")

    files: list[tuple[str, int]] = []
    total_bytes = 0
    for line in res.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 6 and parts[0].startswith("-"):
            try:
                sz = int(parts[4])
                fname = os.path.basename(parts[-1])
                files.append((fname, sz))
                total_bytes += sz
            except (ValueError, IndexError):
                continue
    return files, total_bytes


def empty_stock_resources(host: str, user: str, ssh_opts: list[str]) -> bool:
    """Safely empty all files and subdirectories from /data/resources, leaving empty directory."""
    cmd = (
        'if [ -d /data/resources ]; then '
        '  rm -rf /data/resources/* /data/resources/.* 2>/dev/null || true; '
        '  mkdir -p /data/resources 2>/dev/null || true; '
        '  /data/harmony/bin/hub_sync 2>/dev/null || sync 2>/dev/null || true; '
        'fi'
    )
    res = run_ssh_cmd(host, user, cmd, ssh_opts)
    return res.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Empty Logitech stock resources (/data/resources) on Harmony Hub")
    parser.add_argument("host", nargs="?", default=os.environ.get("HUB_IP"), help="Harmony Hub IP or hostname")
    parser.add_argument("--host", "-H", dest="flag_host", help="Alternative host specification")
    parser.add_argument("--user", default="root", help="SSH username (default: root)")
    parser.add_argument("--key", "-k", help="Path to SSH private key")
    parser.add_argument("--dry-run", "-n", action="store_true", help="Scan and show space to be saved without modifying")
    parser.add_argument("--yes", "-y", action="store_true", help="Skip confirmation prompt")

    args = parser.parse_args()
    hub_ip = args.flag_host or args.host
    if not hub_ip:
        parser.error("Hub host required. Specify <HUB_IP> or set HUB_IP environment variable.")

    ssh_opts = get_ssh_opts(args.key)
    print(f"[*] Inspecting stock resources on Harmony Hub at {hub_ip}...")

    try:
        files, total_bytes = scan_stock_resources(hub_ip, args.user, ssh_opts)
    except Exception as e:
        print(f"[-] Error querying Hub: {e}", file=sys.stderr)
        return 1

    total_kb = (total_bytes + 1023) // 1024 if total_bytes > 0 else 0
    if not files or total_bytes == 0:
        print("[+] /data/resources is already empty (0 files, 0 kB). Nothing to do.")
        return 0

    print(f"[*] Found {len(files)} stock resource file(s) in /data/resources:")
    for fname, sz in sorted(files, key=lambda x: x[1], reverse=True):
        print(f"    - {fname.ljust(25)} {sz:>8} bytes ({sz / 1024:>5.1f} kB)")
    print(f"[+] Total space used by stock resources: {total_bytes:,} bytes (~{total_kb} kB).")

    if args.dry_run:
        print(f"\n[dry-run] Would empty /data/resources and free ~{total_kb} kB on /data. Nothing was changed.")
        return 0

    if not args.yes:
        try:
            reply = input(f"\n[?] Empty all files from /data/resources and free ~{total_kb} kB? [y/N]: ")
        except (EOFError, KeyboardInterrupt):
            print("\nAborted.")
            return 1
        if not reply.strip().lower().startswith("y"):
            print("Aborted.")
            return 1

    print("[*] Emptying /data/resources and synchronizing NOR flash buffers...")
    if not empty_stock_resources(hub_ip, args.user, ssh_opts):
        print("[-] Failed to empty /data/resources on Hub.", file=sys.stderr)
        return 1

    remaining_files, remaining_bytes = scan_stock_resources(hub_ip, args.user, ssh_opts)
    if remaining_files:
        print(f"[!] Warning: {len(remaining_files)} file(s) still remain in /data/resources.", file=sys.stderr)
        return 1

    print(f"[+] Success! /data/resources emptied. Freed ~{total_kb} kB ({total_bytes:,} bytes) on /data.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
