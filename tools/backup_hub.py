#!/usr/bin/env python3
"""Harmony Hub Backup Utility.

Creates golden and rolling tar backups of Harmony Hub configuration over SSH.
Based on build/backup_hub.sh.

Features:
- Auto-detects SSH keys (~/.ssh/harmony_owner_*, id_ed25519)
- Automatically stages RAM BusyBox (/tmp/busybox) if Hub stock BusyBox lacks tar
- Creates an initial GOLDEN backup per Hub (never overwritten)
- Creates timestamped rolling backups of /data/harmony, configs, and system files
- File names carry the Hub's id (its Wi-Fi MAC), so several Hubs can share one folder
- Rotates each Hub's rolling backups: the newest 5, plus the newest of each of the
  8 weeks before them
- Keeps a copy of the stock Logitech files (/data/resources) in stock_resources_<hub>.tar,
  checked file by file against the Hub and never overwritten (files that changed since get a
  new copy beside it), and marks the Hub (/data/harmony/stock_resources_saved) with the copy's
  name and a fingerprint of the files: the stock resources delete needs both (the web UI checks
  the fingerprint, tools/clean_stock_resources.py the copy itself)
"""

from __future__ import annotations

import argparse
import datetime
import glob
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tarfile
from collections.abc import Callable

# Shared SSH host key policy (tools/ssh_hostkeys.py, next to this file)
_HK_DIR = pathlib.Path(__file__).resolve().parent
if str(_HK_DIR) not in sys.path:
    sys.path.append(str(_HK_DIR))
import ssh_hostkeys  # noqa: E402


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

        for std in ("id_ed25519",):  # the Hub server takes Ed25519 keys only
            cand = ssh_dir / std
            if cand.is_file():
                return str(cand)

    return None


def get_ssh_cmd(host: str, user: str = "root", key_path: str | None = None) -> list[str]:
    # LogLevel=ERROR, not -q: a host key failure must still be printed
    cmd = ["ssh", "-o", "LogLevel=ERROR", "-o", "BatchMode=yes", *ssh_hostkeys.ssh_options(host)]
    if key_path and os.path.isfile(key_path):
        cmd.extend(["-i", key_path])
    cmd.append(f"{user}@{host}")
    return cmd


# What the backups hold. Missing paths are skipped by tar (and its exit code is ignored for
# that reason); tar_is_complete() still catches a cut stream.
GOLDEN_PATHS = "/data/resources /etc/passwd /etc/network /etc/TZ /etc/wpa_supplicant.conf /etc/dropbear"
ROLLING_PATHS = ("/data/harmony /data/resources /data/stock_backup /etc/passwd /etc/network /etc/TZ "
                 "/etc/wpa_supplicant.conf /etc/dropbear")

# The stock Logitech files: copies kept for good, and the mark on the Hub ("<copy name>
# <fingerprint>") that says a copy of exactly the files there now exists
STOCK_RES_DIR = "/data/resources"
STOCK_RES_SAVED = "/data/harmony/stock_resources_saved"

Run = Callable[[str], subprocess.CompletedProcess]
# relative path -> (size, md5 hex; "link" for a link)
StockFiles = dict[str, tuple[int, str]]

# Every entry under /data/resources but directories, as "<size> <md5> <path>" ("0 link <path>"
# for a link), "LINK" when /data/resources itself is one. Stock BusyBox 1.13.4 has no find, du,
# wc or "ls -R", and its "ls -l" prints no date: the shell walks the tree, "ls -ld" gives the size.
STOCK_FILES_CMD = (
    'walk() { for f in "$1"/* "$1"/.[!.]* "$1"/..?*; do '
    'if [ -L "$f" ]; then echo "0 link $f"; '
    'elif [ -f "$f" ]; then echo "$(ls -ld "$f" | awk \'{print $5}\') $(md5sum "$f" | awk \'{print $1}\') $f"; '
    'elif [ -d "$f" ]; then walk "$f"; fi; done; }; '
    f'if [ -L {STOCK_RES_DIR} ]; then echo LINK; elif [ -d {STOCK_RES_DIR} ]; then walk {STOCK_RES_DIR}; fi'
)


class LinkError(RuntimeError):
    pass


def ssh_run(ssh_base: list[str]) -> Run:
    return lambda cmd: subprocess.run(ssh_base + [cmd], capture_output=True, text=True)


def hub_stock_files(run: Run) -> StockFiles:
    """The files in /data/resources on the Hub. Raises LinkError when it is a link."""
    res = run(STOCK_FILES_CMD)
    if res.returncode != 0:
        raise RuntimeError(f"listing {STOCK_RES_DIR} failed ({res.returncode}): {(res.stderr or '').strip()}")
    files: StockFiles = {}
    for line in (res.stdout or "").splitlines():
        if line.strip() == "LINK":
            raise LinkError(f"{STOCK_RES_DIR} is a link, not a directory")
        size, _, rest = line.partition(" ")
        digest, _, path = rest.partition(" ")
        if path.startswith(STOCK_RES_DIR + "/"):
            files[path[len(STOCK_RES_DIR) + 1:]] = (int(size) if size.isdigit() else -1, digest)
    return files


def archive_stock_files(path: pathlib.Path) -> StockFiles | None:
    """The files under data/resources in a plain tar, as hub_stock_files() gives them; None when
    path is no complete tar."""
    if not path.is_file() or not tar_is_complete(path):
        return None
    files: StockFiles = {}
    prefix = STOCK_RES_DIR.lstrip("/") + "/"
    try:
        with tarfile.open(path, "r:") as tf:
            for m in tf:
                name = m.name.lstrip("/")
                if not name.startswith(prefix) or m.isdir():
                    continue
                if m.isfile():
                    f = tf.extractfile(m)
                    files[name[len(prefix):]] = (m.size, hashlib.md5(f.read() if f else b"").hexdigest())
                else:
                    files[name[len(prefix):]] = (0, "link")
    except (tarfile.TarError, OSError, EOFError):
        return None
    return files


def stock_fingerprint(files: StockFiles) -> str:
    """"<count>:<hex>", the XOR of sha256("<size> <path>") over the files (any order). The Hub API
    computes the same over /data/resources (http_api_system.c) and compares it with the mark."""
    acc = bytearray(32)
    for rel, (size, _) in files.items():
        for i, b in enumerate(hashlib.sha256(f"{size} {rel}".encode()).digest()):
            acc[i] ^= b
    return f"{len(files)}:{acc.hex()}"


def is_mac(tag: str | None) -> bool:
    return bool(tag and re.fullmatch(r"[0-9a-f]{12}", tag))


def stock_resources_file(backup_dir: pathlib.Path, tag: str) -> pathlib.Path:
    return backup_dir / f"stock_resources_{tag}.tar"


def stock_copies(backup_dir: pathlib.Path, tag: str) -> list[pathlib.Path]:
    """The Hub's copies of its stock files, newest first."""
    copies = [stock_resources_file(backup_dir, tag), *backup_dir.glob(f"stock_resources_{tag}_*.tar")]
    return sorted((p for p in copies if p.is_file()), key=lambda p: p.stat().st_mtime, reverse=True)


def matching_copy(backup_dir: pathlib.Path, tag: str | None, files: StockFiles) -> pathlib.Path | None:
    """The local copy that holds exactly these files (same names, sizes and md5)."""
    if not files or not is_mac(tag):
        return None
    return next((p for p in stock_copies(backup_dir, tag) if archive_stock_files(p) == files), None)


def default_backup_dir(repo_root: pathlib.Path | None = None) -> pathlib.Path:
    """backups/ of the checkout, or of the release bundle this tool is part of."""
    script_dir = pathlib.Path(__file__).resolve().parent
    root = repo_root or script_dir.parent
    if (root / "payload").is_dir():
        return root / "backups"
    if (script_dir / "bin").is_dir():
        return script_dir / "backups"
    return script_dir.parent / "backups"


# Rotation: the newest KEEP_RECENT rolling backups of a Hub, plus the newest one of each of
# the KEEP_WEEKS calendar weeks before them (a week that already has one of the newest
# backups gets no extra one)
KEEP_RECENT = 5
KEEP_WEEKS = 8


def hub_tag(ssh_base: list[str], hub_host: str) -> str:
    """A name part that tells Hubs apart: the Wi-Fi MAC (as the Hub's own id falls back to),
    else the address the tool was given."""
    return hub_mac(ssh_run(ssh_base)) or re.sub(r"[^A-Za-z0-9.-]", "_", hub_host) or "hub"


def hub_mac(run: Run) -> str | None:
    """The Hub's Wi-Fi MAC as 12 hex digits, None when it cannot be read."""
    res = run("cat /sys/class/net/ath0/address 2>/dev/null")
    mac = re.sub(r"[^0-9a-f]", "", (res.stdout or "").strip().lower())
    return mac if res.returncode == 0 and len(mac) == 12 else None


def backups_to_remove(backups: list[pathlib.Path]) -> list[pathlib.Path]:
    """Rolling backups of one Hub that rotation deletes (see KEEP_RECENT, KEEP_WEEKS)."""
    newest_first = sorted(backups, key=lambda p: p.stat().st_mtime, reverse=True)

    def week_of(b: pathlib.Path) -> tuple[int, int]:
        return datetime.date.fromtimestamp(b.stat().st_mtime).isocalendar()[:2]

    keep = set(newest_first[:KEEP_RECENT])
    recent_weeks = {week_of(b) for b in keep}
    weeks_kept: set[tuple[int, int]] = set()
    for b in newest_first[KEEP_RECENT:]:
        week = week_of(b)
        if week not in recent_weeks and week not in weeks_kept and len(weeks_kept) < KEEP_WEEKS:
            weeks_kept.add(week)
            keep.add(b)
    return [b for b in newest_first if b not in keep]


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


def save_stock_resources(ssh_base: list[str], tar_cmd: str, backup_dir: pathlib.Path, tag: str,
                         files: StockFiles, dry_run: bool = False) -> bool:
    """Make sure a local copy holds exactly the Hub's stock files (files: what the Hub has now),
    then mark the Hub with its name and their fingerprint. A copy is never rewritten: changed
    files get a new one beside it. Returns False only when the Hub has files and none could be kept."""
    if not files:
        return True  # nothing to save (emptied before, or a Hub without stock files)
    if not is_mac(tag):
        # A copy named after the address could be taken for another Hub's at that address
        print("[!] The Hub's MAC could not be read: its stock files are not saved and the Hub is not marked.")
        return False
    copy = matching_copy(backup_dir, tag, files)
    if copy is None:
        copy = stock_resources_file(backup_dir, tag)
        if copy.exists():
            copy = backup_dir / f"stock_resources_{tag}_{datetime.datetime.now():%Y%m%d_%H%M%S}.tar"
        if dry_run:
            print(f"    [dry-run] would save the stock files to {copy.name}")
            return True
        print(f"[*] Saving the stock Logitech files ({STOCK_RES_DIR}) to {copy.name}...")
        # tar's exit code counts here: a file it could not read fails the copy
        if not save_archive(ssh_base, f"{tar_cmd} -cf - {STOCK_RES_DIR}", copy):
            return False
        if archive_stock_files(copy) != files:
            print(f"[!] {copy.name} does not hold the Hub's files as listed (changed while saved?); removed, "
                  "the Hub is not marked as saved.")
            copy.unlink(missing_ok=True)
            return False
        print(f"[+] Stock files saved: {copy.name} ({copy.stat().st_size // 1024} KB, kept for good)")
    if dry_run:
        return True
    mark = subprocess.run(
        ssh_base + [f"mkdir -p /data/harmony && echo '{copy.name} {stock_fingerprint(files)}' > {STOCK_RES_SAVED}"],
        capture_output=True, text=True)
    if mark.returncode != 0:
        print(f"[!] Could not mark the Hub ({STOCK_RES_SAVED}): {mark.stderr.strip()}")
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
        backup_dir = default_backup_dir(repo_root)

    backup_dir.mkdir(parents=True, exist_ok=True)

    if ssh_key is None:
        ssh_key = find_default_ssh_key()

    print(f"[*] Connecting to Harmony Hub ({hub_host})...")
    ssh_base = get_ssh_cmd(hub_host, hub_user, ssh_key)

    # The Hub is a MIPS board; the remote (ARM) answers on the same key at 192.168.2.1
    arch = subprocess.run(ssh_base + ["uname -m"], capture_output=True, text=True)
    machine = (arch.stdout or "").strip()
    if arch.returncode != 0 and ssh_hostkeys.report_failure(arch.stderr, hub_host):
        return False
    if arch.returncode != 0 or not machine.startswith("mips"):
        print(f"[-] {hub_host} is not a Harmony Hub (uname -m: {machine or 'no answer'}); nothing was saved. "
              "For the remote use remote/setup_remote.py --backup.")
        return False

    tag = hub_tag(ssh_base, hub_host)
    golden_file = backup_dir / f"golden_hub_backup_{tag}.tar"
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    rolling_file = backup_dir / f"hub_backup_{tag}_{timestamp}.tar"

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
            print("[-] No backup made: the Hub's BusyBox has no tar applet and no MIPS BusyBox could be staged")
            print("    (build one with build/build_busybox.sh, or put it in payload/bin/busybox).")
            return False

    try:
        # 1. Golden backup. Before backups were named per Hub there was one golden_hub_backup.tar,
        # taken at the first deploy (the stock state). It holds nothing that names its Hub, so it
        # is kept as it is, and no new golden backup is made: it would hold today's state, not
        # the stock one, under a name that says otherwise.
        legacy_golden = backup_dir / "golden_hub_backup.tar"
        try:
            stock_files = hub_stock_files(ssh_run(ssh_base))
        except RuntimeError as e:
            print(f"[!] The stock files could not be listed: {e}")
            stock_files = {}
        if not golden_file.is_file() and legacy_golden.is_file():
            print(f"[+] legacy golden backup kept: {legacy_golden.name}")
        elif not golden_file.is_file() and not stock_files:
            # The stock state has its files: a golden backup made now would hold none, under a
            # name that says it is the stock state
            copies = ", ".join(p.name for p in stock_copies(backup_dir, tag)) if is_mac(tag) else ""
            print(f"[!] No golden backup made: /data/resources holds no stock files. Saved copies here: "
                  f"{copies or 'none (see the backups folder of the computer that saved them)'}.")
        elif not golden_file.is_file():
            print(f"[*] Creating the first (GOLDEN) backup of this Hub ({tag})...")
            if dry_run:
                print(f"    [dry-run] would save golden backup to {golden_file}")
            else:
                remote_cmd = f"{tar_cmd} -cf - {GOLDEN_PATHS} 2>/dev/null || true"
                if save_archive(ssh_base, remote_cmd, golden_file):
                    sz_kb = golden_file.stat().st_size // 1024
                    print(f"[+] Golden backup saved to {golden_file.name} ({sz_kb} KB)")
                else:
                    print("[!] Golden backup failed; it will be tried again on the next run.")
        else:
            print(f"[+] Golden backup already exists: {golden_file.name}")

        # 2. The stock files, once per Hub (the way back after the stock resources delete)
        if not save_stock_resources(ssh_base, tar_cmd, backup_dir, tag, stock_files, dry_run):
            print("[!] The stock files could not be saved; the Hub is not marked as saved.")

        # 3. Rolling backup
        print("[*] Creating rolling backup...")
        if dry_run:
            print(f"    [dry-run] would save rolling backup to {rolling_file}")
        else:
            remote_cmd = f"{tar_cmd} -cf - {ROLLING_PATHS} 2>/dev/null || true"
            if save_archive(ssh_base, remote_cmd, rolling_file):
                sz_kb = rolling_file.stat().st_size // 1024
                print(f"[+] Rolling backup saved: {rolling_file.name} ({sz_kb} KB)")
            else:
                print("[!] Rolling backup failed; older backups were kept.")
                return False

        # 4. Rotate this Hub's rolling backups (only archives that passed the check exist)
        stale = backups_to_remove(list(backup_dir.glob(f"hub_backup_{tag}_*.tar")))
        if stale:
            print(f"[*] Rotating backups of {tag} (keeping the newest {KEEP_RECENT} and one per week "
                  f"for {KEEP_WEEKS} weeks)...")
            for old_b in stale:
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
    ssh_hostkeys.add_reset_option(parser, "the Hub")
    args = parser.parse_args()
    # Before any USB or device work: a stop here leaves the device untouched
    ssh_hostkeys.require_known_hosts()
    if args.reset_host_key:
        ssh_hostkeys.reset_host(args.host)

    success = backup_hub(
        hub_host=args.host,
        hub_user=args.user,
        ssh_key=args.key,
        backup_dir=args.backup_dir,
        dry_run=args.dry_run,
    )
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    if sys.version_info < (3, 13):
        sys.exit(f"This tool needs Python 3.13 or newer (this is Python {sys.version.split()[0]}).")
    main()
