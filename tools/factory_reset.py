#!/usr/bin/env python3
"""Harmony Hub & Remote - Factory Reset & Stock Revert Utility.

Safely wipes all custom runtime files, removes persistence scripts,
restores original stock configuration files and permissions, and reboots
the device(s) back into official Logitech stock firmware.

Hardware Factory Reset Alternative (No SSH Required):
    Harmony Hub:
        1. Unplug micro-USB cable from Hub.
        2. Press and hold the 'Pair/Reset' button on the back of the Hub.
        3. Plug micro-USB into PC while holding the button for 15-20 seconds.
        4. Release when the front LED flashes red rapidly.
        5. Restore stock configuration using: python tools/restore_stock.py

    Harmony Remote:
        1. Press and hold 'Menu' (above CH+) and 'Off' (top power button) simultaneously.
        2. Keep holding both buttons for ~20 seconds.
        3. The remote graphic with circular arrow will flash briefly first.
           IMPORTANT: Keep holding both buttons until the graphic stays permanently on the screen.
        4. Release once the graphic stays on screen. The remote wipes all user data and reboots.

Usage:
    python tools/factory_reset.py --target hub --hub-ip 192.168.1.20
    python tools/factory_reset.py --target remote [--remote-ip 192.168.2.1]
    python tools/factory_reset.py --target all --hub-ip 192.168.1.20
    python tools/factory_reset.py --restore-backup

The Hub reset deletes /data/harmony (devices, activities, button maps, Bluetooth keys). A
backup is saved first with tools/backup_hub.py; --no-backup skips it.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import subprocess
import sys
import time

# Shared SSH host key policy (tools/ssh_hostkeys.py, next to this file)
_HK_DIR = pathlib.Path(__file__).resolve().parent
if str(_HK_DIR) not in sys.path:
    sys.path.append(str(_HK_DIR))
import ssh_hostkeys  # noqa: E402

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR.parent

# No default Hub address: a wrong guess (a router at 192.168.1.1) would get the Hub steps
DEFAULT_HUB_IP = os.environ.get("HUB_IP") or None
DEFAULT_REMOTE_IP = "192.168.2.1"


def find_default_ssh_key() -> pathlib.Path | None:
    """Same choice as deploy_hub.py and backup_hub.py: $SSH_KEY, else the newest
    ~/.ssh/harmony_owner_* key."""
    env_key = os.environ.get("SSH_KEY")
    if env_key and os.path.isfile(env_key):
        return pathlib.Path(env_key)
    ssh_dir = pathlib.Path.home() / ".ssh"
    if ssh_dir.is_dir():
        keys = sorted([p for p in ssh_dir.glob("harmony_owner_*") if p.is_file() and not p.name.endswith(".pub")],
                      key=lambda p: p.stat().st_mtime, reverse=True)
        if keys:
            return keys[0]
    return None


def get_ssh_opts(key_path: pathlib.Path | None = None) -> list[str]:
    opts = [
        # Never wait for a password prompt the user cannot see (output is captured)
        "-o", "BatchMode=yes",
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=5",
    ]
    key = key_path if key_path and key_path.is_file() else find_default_ssh_key()
    if key:
        opts.extend(["-i", str(key)])
    return opts


def run_ssh_cmd(target: str, cmd: str, ssh_opts: list[str]) -> subprocess.CompletedProcess[str]:
    # Host key options per target: the Hub and the remote are recorded under their own names
    return subprocess.run(["ssh"] + ssh_hostkeys.ssh_options(target) + ssh_opts + [target, cmd],
                          capture_output=True, text=True)


# Detached and delayed by a second, so the SSH call returns its exit code before the
# reboot drops the connection: a non-zero code then means the command did not get there
REBOOT_CMD = "(sleep 1; reboot -f) </dev/null >/dev/null 2>&1 &"


def device_machine(target: str, ssh_opts: list[str]) -> subprocess.CompletedProcess[str]:
    return run_ssh_cmd(target, "uname -m", ssh_opts)


def reset_hub(hub_ip: str, ssh_opts: list[str], backup: bool = True) -> bool:
    target = f"root@{hub_ip}"
    print(f"\n[*] Probing Harmony Hub at {target}...")
    probe = device_machine(target, ssh_opts)
    if probe.returncode != 0:
        if not ssh_hostkeys.report_failure(probe.stderr, hub_ip):
            print(f"[-] Cannot reach Hub over SSH at {target}: {probe.stderr.strip()}")
        return False
    if not probe.stdout.strip().startswith("mips"):
        print(f"[-] {hub_ip} is not a Harmony Hub (uname -m: {probe.stdout.strip() or 'no answer'}); nothing was changed.")
        return False

    print(f"[+] Connected to Hub ({probe.stdout.strip()})")
    if backup:
        sys.path.insert(0, str(SCRIPT_DIR))
        import backup_hub
        print("[*] Saving a backup of the Hub before /data/harmony is deleted...")
        key = next((ssh_opts[i + 1] for i, o in enumerate(ssh_opts) if o == "-i"), None)
        if not backup_hub.backup_hub(hub_host=hub_ip, ssh_key=key):
            print("[-] No backup was made, so nothing was reset. Run again with --no-backup to reset without one.")
            return False
    print("[*] Performing Hub factory reset sequence:")
    # payload/scripts/factory_reset.sh: stops the services, restores the stock boot files,
    # deletes /data/harmony, the Wi-Fi configuration and SSH access
    return run_reset_script(target, "hub", ssh_opts)


def reset_remote(remote_ip: str, ssh_opts: list[str]) -> bool:
    target = f"root@{remote_ip}"
    print(f"\n[*] Probing Harmony remote at {target}...")
    probe = device_machine(target, ssh_opts)
    if probe.returncode != 0:
        if not ssh_hostkeys.report_failure(probe.stderr, remote_ip):
            print(f"[-] Cannot reach the remote over SSH at {target}: {probe.stderr.strip()}")
            print("    Ensure remote is connected via USB and PC adapter is set to 192.168.2.2.")
        return False
    if not probe.stdout.strip().startswith("arm"):
        print(f"[-] {remote_ip} is not a Harmony remote (uname -m: {probe.stdout.strip() or 'no answer'}); nothing was changed.")
        return False

    print(f"[+] Connected to remote ({probe.stdout.strip()})")
    print("[*] Performing remote factory reset sequence:")
    # remote/factory_reset.sh: stops the runtime, restores the stock boot script, deletes the
    # settings, logs and SSH access
    return run_reset_script(target, "remote", ssh_opts)


def confirm_prompt(device_label: str, backup: bool) -> bool:
    print("\n" + "!" * 70)
    print(f" WARNING: FACTORY RESET FOR {device_label.upper()}")
    print("!" * 70)
    print(" This operation will:")
    print("  1. Remove all custom software and DELETE its settings, accounts, keys, Wi-Fi and SSH access.")
    print("  2. Restore original stock startup scripts and daemon permissions.")
    print("  3. Restore stock watchdog and power management behavior.")
    print("  4. Reboot device(s) back into official Logitech stock mode.")
    if device_label in ("hub", "all"):
        print("  5. DELETE the Hub configuration in /data/harmony: devices, activities,")
        print("     learned codes, button maps and Bluetooth keys, and the Hub's SSH access.")
        print("     " + ("A backup is saved first (tools/backup_hub.py)." if backup else
                         "NO backup is saved (--no-backup)."))
    print("!" * 70)

    val = input(f"\nType 'FACTORY-RESET' to confirm reverting {device_label} to stock: ").strip()
    return val == "FACTORY-RESET"


# Each device has one reset script, the same one its own menu or web UI runs. The tool sends it
# on the standard input of ONE login that saves it to the device's /tmp and runs it without its own
# reboot, then writes a mark and starts the reboot: the script deletes the SSH keys, so a second
# login would be refused. Script, mark and reboot run with HUP and PIPE ignored and their output
# in a file on the device, so a login that is cut does not stop them half way; the login prints
# that file at the end.
RESET_SCRIPTS = {
    "hub": ("Harmony Hub factory reset",
            [REPO_DIR / "payload" / "scripts" / "factory_reset.sh", REPO_DIR / "scripts" / "factory_reset.sh"]),
    "remote": ("Harmony remote factory reset",
               [REPO_DIR / "remote" / "factory_reset.sh", REPO_DIR / "factory_reset.sh"]),
}
DEVICE_RESET_SCRIPT = "/tmp/factory_reset.sh"
DEVICE_RESET_LOG = "/tmp/factory_reset.log"
RESET_DONE_MARK = "factory-reset-steps-done"


def reset_login_cmd(md5: str) -> str:
    """The reset login. The script runs only when it arrived complete (its MD5, computed here, is
    checked on the device: an empty or cut upload is refused before anything changes)."""
    return (f"rm -f {DEVICE_RESET_LOG}; cat > {DEVICE_RESET_SCRIPT}; "
            f"if [ \"$(md5sum < {DEVICE_RESET_SCRIPT} | cut -c1-32)\" != {md5} ]; then "
            f"echo '[-] The reset script did not arrive complete; nothing was changed.'; exit 1; fi; "
            f"(trap '' HUP PIPE; sh {DEVICE_RESET_SCRIPT} --confirm --no-reboot > {DEVICE_RESET_LOG} 2>&1 && "
            f"echo {RESET_DONE_MARK} >> {DEVICE_RESET_LOG} && {{ {REBOOT_CMD} }}); "
            f"cat {DEVICE_RESET_LOG}; grep -q '^{RESET_DONE_MARK}$' {DEVICE_RESET_LOG}")


def reset_script_path(device: str) -> pathlib.Path | None:
    """The reset script of this device next to the tool (repository or release bundle)"""
    marker, candidates = RESET_SCRIPTS[device]
    for c in candidates:
        try:
            if marker in c.read_text(encoding="utf-8", errors="replace")[:400]:
                return c
        except OSError:
            continue
    return None


def run_reset_script(target: str, device: str, ssh_opts: list[str]) -> bool:
    script = reset_script_path(device)
    if script is None:
        print(f"[-] The {device} reset script (factory_reset.sh) is not next to this tool; nothing was changed.")
        return False
    data = script.read_bytes()
    res = subprocess.run(["ssh"] + ssh_opts + [target, reset_login_cmd(hashlib.md5(data).hexdigest())],
                         input=data, capture_output=True, check=False)
    finished = False
    for line in res.stdout.decode(errors="replace").splitlines():
        if line.strip() == RESET_DONE_MARK:
            finished = True
        elif line.strip():
            print(line)
    if finished:
        # the exit code may be lost when the reboot closes the connection first
        print(f"[+] The {device} was reset and reboots into the stock Logitech firmware.")
        return True
    print(f"[-] The factory reset of the {device} was not confirmed (exit code {res.returncode}): "
          f"{res.stderr.decode(errors='replace').strip()}")
    if res.returncode == 255:
        print(f"    The connection was lost. The reset may still have finished: the {device} then reboots by itself")
        print("    within a minute. If it does not come back with the stock firmware and SSH refuses the key,")
        print(f"    power-cycle the {device}.")
    else:
        print(f"    The {device} was not rebooted. Run the tool again; if SSH refuses the key (the reset got as far")
        print(f"    as removing it), power-cycle the {device}.")
    return False


def main() -> None:
    parser = argparse.ArgumentParser(description="Revert Harmony Hub and/or remote to factory stock")
    parser.add_argument("--target", choices=["hub", "remote", "elite", "all"],
                        help="Device to factory reset (required)")
    parser.add_argument("--hub-ip", default=DEFAULT_HUB_IP,
                        help="Hub IP address (required for --target hub/all; default: $HUB_IP)")
    parser.add_argument("--no-backup", action="store_true", help="Reset the Hub without saving a backup first")
    parser.add_argument("--remote-ip", "--elite-ip", dest="remote_ip", default=DEFAULT_REMOTE_IP,
                        help=f"Remote IP address (default: {DEFAULT_REMOTE_IP}; --elite-ip accepted for compatibility)")
    parser.add_argument("--key", type=pathlib.Path, help="Path to SSH private key")
    parser.add_argument("--restore-backup", nargs="?", const="latest", help="Restore stock Hub configuration from backup using all-in-one restore tool")
    parser.add_argument("--force", action="store_true", help="Bypass interactive confirmation prompt")
    ssh_hostkeys.add_reset_option(parser, "each device the tool connects to")
    args = parser.parse_args()
    # Before any USB or device work: a stop here leaves the device untouched
    ssh_hostkeys.require_known_hosts()

    if not args.restore_backup:
        if not args.target:
            parser.error("--target is required (hub, remote or all)")
        if args.target in ("hub", "all") and not args.hub_ip:
            parser.error("--hub-ip (or $HUB_IP) is required to reset the Hub")

    if args.restore_backup:
        restore_script = SCRIPT_DIR / "restore_stock.py"
        restore_cmd = [sys.executable, str(restore_script)]
        if args.restore_backup != "latest":
            restore_cmd.extend(["--backup", args.restore_backup])
        if args.force:
            restore_cmd.append("--force")
        if args.hub_ip:
            restore_cmd.extend(["--hub-ip", args.hub_ip])
        if args.reset_host_key:
            restore_cmd.append(ssh_hostkeys.RESET_OPTION)
        print(f"[*] Delegating to all-in-one restore tool: {' '.join(restore_cmd)}")
        sys.exit(subprocess.run(restore_cmd).returncode)

    print("=" * 70)
    print(" HARMONY SYSTEM FACTORY RESET & STOCK RESTORATION TOOL")
    print(f" Target selection: {args.target.upper()}")
    print("=" * 70)

    if not args.force:
        if not confirm_prompt(args.target, not args.no_backup):
            print("[-] Confirmation string mismatch. Aborting factory reset.")
            sys.exit(1)

    ssh_opts = get_ssh_opts(args.key)
    if args.reset_host_key:
        if args.target in ("hub", "all"):
            ssh_hostkeys.reset_host(args.hub_ip)
        if args.target in ("remote", "elite", "all"):
            ssh_hostkeys.reset_host(args.remote_ip)

    failed = []
    if args.target in ("hub", "all"):
        print(f"\n>>> PROCESSING HUB ({args.hub_ip}) <<<")
        if not reset_hub(args.hub_ip, ssh_opts, backup=not args.no_backup):
            failed.append(f"Hub ({args.hub_ip})")

    if args.target in ("remote", "elite", "all"):  # elite: the name before the rename
        print(f"\n>>> PROCESSING REMOTE ({args.remote_ip}) <<<")
        if not reset_remote(args.remote_ip, ssh_opts):
            failed.append(f"remote ({args.remote_ip})")

    print("\n" + "=" * 70)
    if failed:
        print(f"[-] Factory reset NOT completed for: {', '.join(failed)}")
        print("=" * 70 + "\n")
        sys.exit(1)
    print("[+] Factory reset process completed successfully!")
    print("=" * 70 + "\n")


if __name__ == "__main__":
    if sys.version_info < (3, 13):
        sys.exit(f"This tool needs Python 3.13 or newer (this is Python {sys.version.split()[0]}).")
    main()
