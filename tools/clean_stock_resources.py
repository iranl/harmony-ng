#!/usr/bin/env python3
"""Empty Logitech Stock Resources (/data/resources) on Harmony Hub.

Logitech stock device, activity, and protocol profiles stored in /data/resources
are unused by HarmonyNG once they are imported (/data/harmony/config/devices.json exists).
This tool removes /data/resources with everything in it, creates it again empty and
flushes flash buffers. build/deploy_hub.py --clean-stock-resources uses the same steps.

The delete cannot be undone on the Hub, and the stock firmware needs these files
(Context.json). So it runs only when a saved copy exists: tools/backup_hub.py (which
build/deploy_hub.py runs before every deploy) saves the files once per Hub in
backups/stock_resources_<hub>.tar and marks the Hub (/data/harmony/stock_resources_saved).
Without the mark this tool runs that backup first. tools/restore_stock.py puts the files
back when the archive it restores has none.

The sizes shown are the files' own sizes: /data is JFFS2, which compresses, so the flash
space freed is smaller.

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
from collections.abc import Callable

# Shared SSH host key policy and backups (tools/ssh_hostkeys.py, tools/backup_hub.py, next to this file)
SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import backup_hub  # noqa: E402
import ssh_hostkeys  # noqa: E402

Run = Callable[[str], subprocess.CompletedProcess]
RES_DIR = "/data/resources"

# Stock BusyBox (1.13.4) has no find, du, wc or "ls -R", and its "ls -l" prints no date: walk the
# tree in the shell and take the size from "ls -ld". A link at /data/resources is reported, not walked.
SCAN_CMD = (
    'walk() { for f in "$1"/* "$1"/.[!.]* "$1"/..?*; do '
    'if [ -L "$f" ] || [ -f "$f" ]; then echo "$(ls -ld "$f" | awk \'{print $5}\') $f"; '
    'elif [ -d "$f" ]; then walk "$f"; fi; done; }; '
    f'if [ -L {RES_DIR} ]; then echo LINK; elif [ -d {RES_DIR} ]; then walk {RES_DIR}; fi'
)

# Remove the directory itself: a "/data/resources/.*" glob would also match "..", that is /data.
# rm and mkdir must succeed; only the sync chain may fail (stock BusyBox has no sync applet).
EMPTY_CMD = (
    f"[ ! -L {RES_DIR} ] && rm -rf {RES_DIR} && mkdir {RES_DIR} && "
    "{ /data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null; true; }"
)

STATE_CMD = (f"[ -s /data/harmony/config/devices.json ] && echo imported; "
             f"[ -f {backup_hub.STOCK_RES_SAVED} ] && echo \"saved $(cat {backup_hub.STOCK_RES_SAVED})\"; true")


class LinkError(RuntimeError):
    pass


def ssh_runner(host: str, user: str, ssh_opts: list[str]) -> Run:
    def run(cmd: str) -> subprocess.CompletedProcess:
        return subprocess.run(["ssh"] + ssh_hostkeys.ssh_options(host) + ssh_opts + [f"{user}@{host}", cmd],
                              capture_output=True, text=True, timeout=60)
    return run


def get_ssh_opts(key_path: str | None = None) -> list[str]:
    opts = ["-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=5"]
    key = key_path or backup_hub.find_default_ssh_key()
    if key and os.path.isfile(key):
        opts.extend(["-i", key])
    return opts


def checked(run: Run, cmd: str) -> str:
    res = run(cmd)
    if res.returncode != 0:
        raise RuntimeError(f"SSH command failed ({res.returncode}): {(res.stderr or res.stdout or '').strip()}")
    return res.stdout or ""


def scan_stock_resources(run: Run) -> tuple[list[tuple[str, int]], int]:
    """Every file under /data/resources (subdirectories too) as (relative path, size) and the total
    bytes. Raises LinkError when /data/resources is a link."""
    files: list[tuple[str, int]] = []
    for line in checked(run, SCAN_CMD).splitlines():
        if line.strip() == "LINK":
            raise LinkError(f"{RES_DIR} is a link, not a directory: nothing is removed")
        size, _, path = line.partition(" ")
        if not path.startswith(RES_DIR + "/"):
            continue
        try:
            files.append((path[len(RES_DIR) + 1:], int(size)))
        except ValueError:
            files.append((path[len(RES_DIR) + 1:], 0))
    return files, sum(sz for _, sz in files)


def hub_state(run: Run) -> tuple[bool, str]:
    """(stock configuration imported, name of the saved stock files archive or "")."""
    imported, saved = False, ""
    for line in checked(run, STATE_CMD).splitlines():
        if line == "imported":
            imported = True
        elif line.startswith("saved "):
            saved = line[len("saved "):].strip() or "?"
    return imported, saved


def refusal(run: Run) -> str | None:
    """Why the files must stay (None: they may go)."""
    imported, saved = hub_state(run)
    if not imported:
        return ("/data/harmony/config/devices.json is missing or empty: the stock configuration is not "
                "imported yet (build/deploy_hub.py imports it)")
    if not saved:
        return ("no saved copy of the stock files: run tools/backup_hub.py (build/deploy_hub.py runs it) "
                "first, it saves them in backups/stock_resources_<hub>.tar")
    return None


def empty_stock_resources(run: Run) -> str | None:
    """Remove /data/resources, create it again empty, sync, and check. Returns an error text or None."""
    res = run(EMPTY_CMD)
    if res.returncode != 0:
        return f"removing {RES_DIR} failed (exit code {res.returncode}): {(res.stderr or '').strip() or 'no message'}"
    if checked(run, f"[ -d {RES_DIR} ] && [ ! -L {RES_DIR} ] && echo ok; true").strip() != "ok":
        return f"{RES_DIR} is missing after the delete"
    left, _ = scan_stock_resources(run)
    if left:
        return f"{len(left)} file(s) still in {RES_DIR}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Empty Logitech stock resources (/data/resources) on Harmony Hub")
    parser.add_argument("host", nargs="?", default=os.environ.get("HUB_IP"), help="Harmony Hub IP or hostname")
    parser.add_argument("--host", "-H", dest="flag_host", help="Alternative host specification")
    parser.add_argument("--user", default="root", help="SSH username (default: root)")
    parser.add_argument("--key", "-k", help="Path to SSH private key")
    parser.add_argument("--dry-run", "-n", action="store_true", help="Scan and show what would be removed without changes")
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

    run = ssh_runner(hub_ip, args.user, get_ssh_opts(args.key))
    print(f"[*] Inspecting stock resources on Harmony Hub at {hub_ip}...")
    probe = run("uname -m")
    if probe.returncode != 0:
        if not ssh_hostkeys.report_failure(probe.stderr, hub_ip):
            print(f"[-] Cannot reach the Hub over SSH: {(probe.stderr or '').strip()}", file=sys.stderr)
        return 1
    if not (probe.stdout or "").strip().startswith("mips"):
        print(f"[-] {hub_ip} is not a Harmony Hub (uname -m: {(probe.stdout or '').strip()}); nothing was changed.",
              file=sys.stderr)
        return 1

    try:
        files, total_bytes = scan_stock_resources(run)
    except (RuntimeError, subprocess.SubprocessError) as e:
        print(f"[-] {e}", file=sys.stderr)
        return 1

    if not files:
        print("[+] /data/resources is already empty. Nothing to do.")
        return 0

    total_kb = (total_bytes + 1023) // 1024
    print(f"[*] Found {len(files)} stock resource file(s) in /data/resources:")
    for fname, sz in sorted(files, key=lambda x: x[1], reverse=True):
        print(f"    - {fname.ljust(25)} {sz:>8} bytes ({sz / 1024:>5.1f} kB)")
    print(f"[+] Files: {total_bytes:,} bytes (~{total_kb} kB). /data compresses, so less flash space is freed.")

    try:
        reason = refusal(run)
        if reason and "no saved copy" in reason and not args.dry_run:
            print("[*] No saved copy of the stock files yet: running the backup first...")
            backup_hub.backup_hub(hub_host=hub_ip, hub_user=args.user, ssh_key=args.key)
            reason = refusal(run)
        _, saved = hub_state(run)
    except (RuntimeError, subprocess.SubprocessError) as e:
        print(f"[-] Error querying Hub: {e}", file=sys.stderr)
        return 1
    if reason:
        print(f"[-] {reason}. Nothing was changed.", file=sys.stderr)
        return 1
    print(f"[+] Saved copy of the stock files: {saved} (tools/restore_stock.py puts them back on a revert to stock).")

    if args.dry_run:
        print("\n[dry-run] Would empty /data/resources. Nothing was changed.")
        return 0

    if not args.yes:
        try:
            reply = input(f"\n[?] Remove all {len(files)} file(s) from /data/resources? [y/N]: ")
        except (EOFError, KeyboardInterrupt):
            print("\nAborted.")
            return 1
        if not reply.strip().lower().startswith("y"):
            print("Aborted.")
            return 1

    print("[*] Emptying /data/resources and synchronizing NOR flash buffers...")
    try:
        err = empty_stock_resources(run)
    except (RuntimeError, subprocess.SubprocessError) as e:
        err = str(e)
    if err:
        print(f"[-] {err}", file=sys.stderr)
        return 1
    print(f"[+] /data/resources emptied ({len(files)} file(s), {total_bytes:,} bytes).")
    return 0


if __name__ == "__main__":
    if sys.version_info < (3, 13):
        sys.exit(f"This tool needs Python 3.13 or newer (this is Python {sys.version.split()[0]}).")
    sys.exit(main())
