#!/usr/bin/env python3
"""Empty Logitech Stock Resources (/data/resources) on Harmony Hub.

Logitech stock device, activity, and protocol profiles stored in /data/resources
are unused by HarmonyNG once they are imported (/data/harmony/config/devices.json exists).
This tool removes /data/resources with everything in it, creates it again empty,
flushes flash buffers, and reports reclaimed space.

The stock firmware needs these files (Context.json): after emptying, a revert to stock
needs the golden backup that tools/backup_hub.py made before the first deploy
(tools/restore_stock.py --backup backups/golden_hub_backup_<hub>.tar).

Usage:
    python tools/clean_stock_resources.py <HUB_IP> [--dry-run] [--yes]
    python tools/clean_stock_resources.py 10.234.234.98 --dry-run
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys

# Shared SSH host key policy (tools/ssh_hostkeys.py, next to this file)
SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import ssh_hostkeys  # noqa: E402

# Remove the directory itself: a "/data/resources/.*" glob would also match "..", that is /data
EMPTY_CMD = (
    "rm -rf /data/resources; mkdir -p /data/resources; "
    "/data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true"
)


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
    opts = ["-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=5"]
    key = key_path or find_default_ssh_key()
    if key and os.path.isfile(key):
        opts.extend(["-i", key])
    return opts


def run_ssh_cmd(host: str, user: str, cmd: str, ssh_opts: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["ssh"] + ssh_hostkeys.ssh_options(host) + ssh_opts + [f"{user}@{host}", cmd],
                          capture_output=True, text=True, timeout=30)


def ssh_checked(host: str, user: str, cmd: str, ssh_opts: list[str]) -> str:
    res = run_ssh_cmd(host, user, cmd, ssh_opts)
    if res.returncode != 0:
        if ssh_hostkeys.report_failure(res.stderr, host):
            raise RuntimeError("SSH host key mismatch")
        raise RuntimeError(f"SSH command failed ({res.returncode}): {res.stderr.strip() or res.stdout.strip()}")
    return res.stdout


def scan_stock_resources(host: str, user: str, ssh_opts: list[str]) -> tuple[list[tuple[str, int]], int]:
    """List every file under /data/resources (subdirectories too) as (relative path, size) and the total bytes.
    Stock BusyBox has no find or du: parse "ls -lAR"."""
    out = ssh_checked(host, user, "if [ -d /data/resources ]; then ls -lAR /data/resources 2>/dev/null; fi", ssh_opts)
    files: list[tuple[str, int]] = []
    total_bytes = 0
    subdir = ""
    for line in out.splitlines():
        line = line.strip()
        if line.endswith(":") and line.startswith("/data/resources"):
            subdir = line[len("/data/resources"):-1].strip("/")
            continue
        parts = line.split(None, 8)
        if len(parts) < 6 or parts[0][:1] not in ("-", "l"):
            continue
        try:
            sz = int(parts[4])
        except ValueError:
            continue
        # "perms links user group size month day time|year name": the name may hold spaces
        name = (parts[8] if len(parts) == 9 else parts[-1]).split(" -> ")[0]
        files.append((f"{subdir}/{name}" if subdir else name, sz))
        total_bytes += sz
    return files, total_bytes


def has_migrated_config(host: str, user: str, ssh_opts: list[str]) -> bool:
    out = ssh_checked(host, user, "[ -f /data/harmony/config/devices.json ] && echo 1 || echo 0", ssh_opts)
    return out.strip() == "1"


def empty_stock_resources(host: str, user: str, ssh_opts: list[str]) -> bool:
    """Remove /data/resources with its contents, create it again empty, and sync the flash."""
    return run_ssh_cmd(host, user, EMPTY_CMD, ssh_opts).returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Empty Logitech stock resources (/data/resources) on Harmony Hub")
    parser.add_argument("host", nargs="?", default=os.environ.get("HUB_IP"), help="Harmony Hub IP or hostname")
    parser.add_argument("--host", "-H", dest="flag_host", help="Alternative host specification")
    parser.add_argument("--user", default="root", help="SSH username (default: root)")
    parser.add_argument("--key", "-k", help="Path to SSH private key")
    parser.add_argument("--dry-run", "-n", action="store_true", help="Scan and show space to be saved without modifying")
    parser.add_argument("--yes", "-y", action="store_true", help="Skip confirmation prompt")
    ssh_hostkeys.add_reset_option(parser, "the Hub")

    args = parser.parse_args()
    hub_ip = args.flag_host or args.host
    if not hub_ip:
        parser.error("Hub host required. Specify <HUB_IP> or set HUB_IP environment variable.")
    # Before any device work: a stop here leaves the Hub untouched
    ssh_hostkeys.require_known_hosts()
    if args.reset_host_key:
        ssh_hostkeys.reset_host(hub_ip)

    ssh_opts = get_ssh_opts(args.key)
    print(f"[*] Inspecting stock resources on Harmony Hub at {hub_ip}...")

    try:
        files, total_bytes = scan_stock_resources(hub_ip, args.user, ssh_opts)
        migrated = has_migrated_config(hub_ip, args.user, ssh_opts)
    except Exception as e:
        print(f"[-] Error querying Hub: {e}", file=sys.stderr)
        return 1

    total_kb = (total_bytes + 1023) // 1024
    if not files:
        print("[+] /data/resources is already empty (0 files, 0 kB). Nothing to do.")
        return 0

    print(f"[*] Found {len(files)} stock resource file(s) in /data/resources:")
    for fname, sz in sorted(files, key=lambda x: x[1], reverse=True):
        print(f"    - {fname.ljust(25)} {sz:>8} bytes ({sz / 1024:>5.1f} kB)")
    print(f"[+] Total space used by stock resources: {total_bytes:,} bytes (~{total_kb} kB).")

    if not migrated:
        print("[-] /data/harmony/config/devices.json is missing: the stock configuration is not imported yet. "
              "Run build/deploy_hub.py first. Nothing was changed.", file=sys.stderr)
        return 1

    if args.dry_run:
        print(f"\n[dry-run] Would empty /data/resources and free ~{total_kb} kB on /data. Nothing was changed.")
        return 0

    print("[!] The stock firmware needs these files. After this, a revert to stock needs the golden backup\n"
          "    (backups/golden_hub_backup_<hub>.tar, made by tools/backup_hub.py before the first deploy).")
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

    remaining_files, _ = scan_stock_resources(hub_ip, args.user, ssh_opts)
    if remaining_files:
        print(f"[!] Warning: {len(remaining_files)} file(s) still remain in /data/resources.", file=sys.stderr)
        return 1

    print(f"[+] Success! /data/resources emptied. Freed ~{total_kb} kB ({total_bytes:,} bytes) on /data.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
