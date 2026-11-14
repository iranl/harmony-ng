#!/usr/bin/env python3
"""Harmony Hub Backup Utility.

Creates golden and rolling tar backups of Harmony Hub configuration over SSH.
Based on build/backup_hub.sh.

Features:
- Auto-detects SSH keys (~/.ssh/harmony_owner_*, id_rsa, id_ed25519)
- Automatically stages RAM BusyBox (/tmp/busybox) if Hub stock BusyBox lacks tar
- Creates initial GOLDEN backup of stock state (never overwritten)
- Creates timestamped rolling backups of /data/harmony, configs, and system files
- Automatically rotates rolling backups to retain newest 5
"""

from __future__ import annotations

import argparse
import datetime
import glob
import os
import pathlib
import subprocess
import sys
import tarfile


def find_default_ssh_key() -> str | None:
    """Find newest harmony_owner key or standard SSH key."""
    env_key = os.environ.get("SSH_KEY")
    if env_key and os.path.isfile(env_key):
        return env_key

    home = pathlib.Path.home()
    ssh_dir = home / ".ssh"
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


def get_ssh_cmd(host: str, user: str = "root", key_path: str | None = None) -> list[str]:
    cmd = ["ssh", "-q", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no"]
    if key_path and os.path.isfile(key_path):
        cmd.extend(["-i", key_path])
    cmd.append(f"{user}@{host}")
    return cmd


def tar_is_complete(path: pathlib.Path) -> bool:
    """True when every member can be read and the archive ends with tar's end-of-archive
    blocks. A stream cut by a dropped connection fails one of the two, also when the cut
    falls on a block boundary, which tarfile alone reads as a shorter, valid archive."""
    try:
        with tarfile.open(path, "r:") as tf:
            for member in tf:
                if member.isfile():
                    f = tf.extractfile(member)
                    while f and f.read(65536):
                        pass
            end = tf.offset
        with open(path, "rb") as f:
            f.seek(end)
            trailer = f.read(1024)
        return len(trailer) == 1024 and trailer == bytes(1024)
    except (tarfile.TarError, OSError, EOFError):
        return False


def save_archive(ssh_base: list[str], remote_cmd: str, dest: pathlib.Path) -> bool:
    """Stream a tar from the Hub into dest. The file is written under a temporary name and
    gets its final name only when ssh succeeded and the archive is complete."""
    part = dest.with_name(dest.name + ".part")
    with open(part, "wb") as f_out:
        res = subprocess.run(ssh_base + [remote_cmd], stdout=f_out, stderr=subprocess.PIPE)
    if res.returncode != 0 or not tar_is_complete(part):
        reason = f"ssh exit code {res.returncode}" if res.returncode != 0 else "archive incomplete or unreadable"
        print(f"[!] Backup to {dest.name} failed ({reason}); nothing was saved.")
        part.unlink(missing_ok=True)
        return False
    part.replace(dest)
    return True


def backup_hub(
    hub_host: str,
    hub_user: str = "root",
    ssh_key: str | None = None,
    backup_dir: pathlib.Path | None = None,
    repo_root: pathlib.Path | None = None,
    dry_run: bool = False,
) -> bool:
    """Run golden and rolling backup on Hub."""
    script_dir = pathlib.Path(__file__).resolve().parent
    if repo_root is None:
        repo_root = script_dir.parent

    if backup_dir is None:
        if (repo_root / "payload").is_dir():
            backup_dir = repo_root / "backups"
        elif (script_dir / "bin").is_dir():
            backup_dir = script_dir / "backups"
        else:
            backup_dir = script_dir.parent / "backups"

    backup_dir.mkdir(parents=True, exist_ok=True)
    golden_file = backup_dir / "golden_hub_backup.tar"
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    rolling_file = backup_dir / f"hub_backup_{timestamp}.tar"

    if ssh_key is None:
        ssh_key = find_default_ssh_key()

    print(f"[*] Connecting to Harmony Hub ({hub_host})...")
    ssh_base = get_ssh_cmd(hub_host, hub_user, ssh_key)

    # Detect tar binary on Hub
    tar_cmd = "tar"
    res = subprocess.run(ssh_base + ["test -x /cache/bin/busybox"], capture_output=True)
    if res.returncode == 0:
        tar_cmd = "/cache/bin/busybox tar"
    else:
        res = subprocess.run(ssh_base + ["test -x /data/harmony/bin/busybox"], capture_output=True)
        if res.returncode == 0:
            tar_cmd = "/data/harmony/bin/busybox tar"

    # Test if tar works
    res = subprocess.run(ssh_base + [f"{tar_cmd} --help"], capture_output=True)
    staged_ram_bb = False
    if res.returncode != 0:
        # Stock BusyBox lacks tar: stage local MIPS BusyBox in RAM (/tmp)
        local_bb = None
        for cand in [
            repo_root / "build" / "bin" / "busybox",
            repo_root / "payload" / "bin" / "busybox",
            repo_root / "bin" / "busybox",
        ]:
            if cand.is_file():
                local_bb = cand
                break

        if local_bb and not dry_run:
            print(f"[*] Hub BusyBox has no tar: staging {local_bb.name} to /tmp/busybox (RAM)...")
            try:
                with open(local_bb, "rb") as f_in:
                    stage_res = subprocess.run(
                        ssh_base + ["cat > /tmp/busybox && chmod 755 /tmp/busybox"],
                        stdin=f_in,
                        capture_output=True,
                    )
                if stage_res.returncode == 0:
                    test_tar = subprocess.run(ssh_base + ["/tmp/busybox tar --help"], capture_output=True)
                    if test_tar.returncode == 0:
                        tar_cmd = "/tmp/busybox tar"
                        staged_ram_bb = True
            except Exception as e:
                print(f"[!] Warning during RAM BusyBox staging: {e}")

        if not staged_ram_bb and not dry_run:
            print("[!] Notice: tar applet not found on Hub BusyBox and no local MIPS BusyBox to stage.")
            print("    Skipping pre-deployment backup.")
            return True

    try:
        # 1. Golden backup
        if not golden_file.is_file():
            print("[*] Creating GOLDEN initial backup of Hub stock state...")
            if dry_run:
                print(f"    [dry-run] would save golden backup to {golden_file}")
            else:
                remote_cmd = f"{tar_cmd} -cf - /data/resources /etc/passwd /etc/network /etc/TZ 2>/dev/null || true"
                if save_archive(ssh_base, remote_cmd, golden_file):
                    sz_kb = golden_file.stat().st_size // 1024
                    print(f"[+] Golden backup saved to {golden_file.name} ({sz_kb} KB)")
                else:
                    print("[!] Golden backup failed; it will be tried again on the next run.")
        else:
            print(f"[+] Golden backup already exists: {golden_file.name}")

        # 2. Rolling backup
        print("[*] Creating rolling backup...")
        if dry_run:
            print(f"    [dry-run] would save rolling backup to {rolling_file}")
        else:
            remote_cmd = f"{tar_cmd} -cf - /data/harmony /data/resources /etc/passwd /etc/network /etc/TZ 2>/dev/null || true"
            if save_archive(ssh_base, remote_cmd, rolling_file):
                sz_kb = rolling_file.stat().st_size // 1024
                print(f"[+] Rolling backup saved: {rolling_file.name} ({sz_kb} KB)")
            else:
                print("[!] Rolling backup failed; older backups were kept.")
                return False

        # 3. Rotate rolling backups (keep newest 5)
        rolling_backups = sorted(
            backup_dir.glob("hub_backup_*.tar"),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if len(rolling_backups) > 5:
            print("[*] Rotating backups (keeping newest 5)...")
            for old_b in rolling_backups[5:]:
                if not dry_run:
                    old_b.unlink(missing_ok=True)
                print(f"    Removed old backup: {old_b.name}")

        return True
    finally:
        if staged_ram_bb and not dry_run:
            subprocess.run(ssh_base + ["rm -f /tmp/busybox 2>/dev/null || true"], capture_output=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="Create tar backups of Logitech Harmony Hub over SSH")
    parser.add_argument("host", nargs="?", default="192.168.1.1", help="Harmony Hub IP address (default: 192.168.1.1)")
    parser.add_argument("--user", default="root", help="SSH username (default: root)")
    parser.add_argument("--key", help="Path to SSH private key")
    parser.add_argument("--backup-dir", type=pathlib.Path, help="Custom local backup directory")
    parser.add_argument("--dry-run", action="store_true", help="Report what would be backed up without saving files")
    args = parser.parse_args()

    success = backup_hub(
        hub_host=args.host,
        hub_user=args.user,
        ssh_key=args.key,
        backup_dir=args.backup_dir,
        dry_run=args.dry_run,
    )
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
