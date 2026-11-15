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
import os
import pathlib
import subprocess
import sys
import time

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
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=" + ("NUL" if sys.platform == "win32" else "/dev/null"),
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=5",
    ]
    key = key_path if key_path and key_path.is_file() else find_default_ssh_key()
    if key:
        opts.extend(["-i", str(key)])
    return opts


def run_ssh_cmd(target: str, cmd: str, ssh_opts: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["ssh"] + ssh_opts + [target, cmd], capture_output=True, text=True)


# Detached and delayed by a second, so the SSH call returns its exit code before the
# reboot drops the connection: a non-zero code then means the command did not get there
REBOOT_CMD = "(sleep 1; reboot -f) </dev/null >/dev/null 2>&1 &"


STEP_MARK = "factory-reset-step"
DONE_MARK = "factory-reset-steps-done"


def reset_script(commands: list[str]) -> str:
    """All steps and the reboot as one command, for one SSH login. On the remote the first
    step stops the runtime, and with it the feeding of the 10 s hardware watchdog: separate
    logins for each step could outlast it. Each step prints a mark when it has run; the
    reboot comes only after the last one."""
    parts = [f"{c}; echo {STEP_MARK} {i}" for i, c in enumerate(commands, 1)]
    return "; ".join(parts + [f"echo {DONE_MARK}", REBOOT_CMD])


def run_steps(target: str, commands: list[str], ssh_opts: list[str], device: str) -> bool:
    """Run every reset step and the reboot in one SSH login (see reset_script()). Every step
    ends in "|| true"; a missing mark means the login ended before that step."""
    res = run_ssh_cmd(target, reset_script(commands), ssh_opts)
    done = 0
    finished = False
    for line in res.stdout.splitlines():
        if line.startswith(STEP_MARK + " "):
            done = int(line.split()[-1])
        elif line.strip() == DONE_MARK:
            finished = True
        elif line.strip():
            print(line)
    if not finished or res.returncode != 0:
        print(f"[-] The {device} ran {done} of {len(commands)} reset steps (exit code {res.returncode}): {res.stderr.strip()}")
        print(f"    The factory reset of the {device} is INCOMPLETE and it was not rebooted by this tool. Run the tool again.")
        return False
    print(f"[*] Rebooting the {device} into stock firmware...")
    return True


def device_machine(target: str, ssh_opts: list[str]) -> subprocess.CompletedProcess[str]:
    return run_ssh_cmd(target, "uname -m", ssh_opts)


def reset_hub(hub_ip: str, ssh_opts: list[str], backup: bool = True) -> bool:
    target = f"root@{hub_ip}"
    print(f"\n[*] Probing Harmony Hub at {target}...")
    probe = device_machine(target, ssh_opts)
    if probe.returncode != 0:
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

    commands = [
        # 1. Stop all daemons
        "killall -9 init.sh hub_daemon hub_btstack hub_rf hub_dhcpd hub_portal hal luaworks luadraws lua 2>/dev/null || true",
        # 2. Restore stock rcS.local if backup exists
        (
            "if [ -f /data/stock_backup/rcS.local.orig ]; then "
            "  cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "  echo '  + Restored original stock rcS.local'; "
            "else "
            "  rm -f /etc/init.d/rcS.local; "
            "  echo '  + Removed custom rcS.local (clean boot)'; "
            "fi"
        ),
        # 3. Restore executable permissions to stock daemons
        (
            "chmod +x /usr/bin/watchdog /opt/luaworks/luaworks /usr/bin/hal /usr/sbin/bluetoothd 2>/dev/null || true; "
            "echo '  + Restored permissions on stock daemons (watchdog, hal, luaworks, bluetoothd)'"
        ),
        # 4. Remove watchdog neuter and custom binaries
        (
            "rm -f /etc/nowatchdog /usr/sbin/dropbear /usr/sbin/dropbearkey /data/rf_pair_test /data/test_hub_tx /data/hub_rf 2>/dev/null || true; "
            "rm -rf /data/harmony /data/harmony-backups /cache/*.log /cache/crashlog-*.json 2>/dev/null || true; "
            "echo '  + Removed all custom binaries, scripts, and temporary state'"
        ),

        # 5. Flush filesystem writes
        "sync 2>/dev/null || /data/harmony/bin/hub_sync 2>/dev/null || /data/sync 2>/dev/null || true",
    ]

    if not run_steps(target, commands, ssh_opts, "Hub"):
        return False
    print("[+] Hub reboot signal delivered. Hub will boot into stock Logitech environment.")
    return True


def reset_remote(remote_ip: str, ssh_opts: list[str]) -> bool:
    target = f"root@{remote_ip}"
    print(f"\n[*] Probing Harmony remote at {target}...")
    probe = device_machine(target, ssh_opts)
    if probe.returncode != 0:
        print(f"[-] Cannot reach the remote over SSH at {target}: {probe.stderr.strip()}")
        print("    Ensure remote is connected via USB and PC adapter is set to 192.168.2.2.")
        return False
    if not probe.stdout.strip().startswith("arm"):
        print(f"[-] {remote_ip} is not a Harmony remote (uname -m: {probe.stdout.strip() or 'no answer'}); nothing was changed.")
        return False

    print(f"[+] Connected to remote ({probe.stdout.strip()})")
    print("[*] Performing remote factory reset sequence:")

    commands = [
        # 1. Stop remote runtime & supervisors
        "if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; fi; killall -9 init.sh remote_ui elite_ui remote_watchdog.sh elite_watchdog.sh 2>/dev/null || true",
        # 2. Restore stock rcS.local
        (
            "if [ -f /data/stock_backup/rcS.local.orig ] && ! grep -q 'usbeth' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'dropbear' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'exit 0' /data/stock_backup/rcS.local.orig 2>/dev/null; then "
            "  cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "  echo '  + Restored original stock rcS.local'; "
            "else "
            "  printf '#!/bin/sh\\n/usr/bin/flashuboot /lib/firmware/u-boot.bin 2>/dev/null || true\\n' > /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "  echo '  + Restored clean stock rcS.local'; "
            "fi"
        ),
        # 3. Restore executable permissions to stock daemons
        (
            "chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true; "
            "echo '  + Restored permissions on stock daemons (usbhid, hal, luaworks, luadraws, watchdog, bluetoothd, dbus-daemon)'"
        ),
        # 4. Remove custom triggers, dangling symlinks, and files
        (
            "rm -f /etc/nowatchdog /etc/tdeenable /data/reboot_counter /data/remote_ui.conf /tmp/remote_ui.log /tmp/remote-init.log /tmp/remote_stop /tmp/remote_supervisors.pid 2>/dev/null || true; "
            "rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true; "
            "rm -f /usr/bin/head /usr/bin/tail /usr/bin/which /usr/bin/find /usr/bin/tar /usr/bin/xargs /usr/bin/xz /usr/bin/bzcat /usr/bin/bunzip2 /usr/bin/lzma /usr/bin/unlzma /usr/bin/diff /usr/bin/patch /usr/bin/awk /usr/bin/sed 2>/dev/null || true; "
            "/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true; sleep 1; "
            "rm -f /data/remote_* /data/elite_* /data/init.sh /data/safe_reboot.sh /data/factory_reset.sh /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox 2>/dev/null || true; "
            "rm -rf /data/stock_backup /data/assets 2>/dev/null || true; "
            "echo '  + Removed all custom binaries, Dropbear symlinks, and configuration files'"
        ),
        # 5. Flush filesystem writes (our sync binary is gone by now; stock firmware
        #    has no sync link in PATH, only the BusyBox applet)
        "/bin/busybox sync 2>/dev/null || sync 2>/dev/null || true; sleep 1",
    ]

    if not run_steps(target, commands, ssh_opts, "remote"):
        return False
    print("[+] Remote reboot signal delivered. Remote will boot into stock Logitech UI.")
    return True


def confirm_prompt(device_label: str, backup: bool) -> bool:
    print("\n" + "!" * 70)
    print(f" WARNING: FACTORY RESET FOR {device_label.upper()}")
    print("!" * 70)
    print(" This operation will:")
    print("  1. Remove all custom software, binaries, and Web UI.")
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
    args = parser.parse_args()

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
    if sys.version_info < (3, 11):
        sys.exit(f"This tool needs Python 3.11 or newer (this is Python {sys.version.split()[0]}).")
    main()
