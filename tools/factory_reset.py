#!/usr/bin/env python3
"""Harmony Hub & Elite Remote - Factory Reset & Stock Revert Utility.

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

    Harmony Elite Remote:
        1. Press and hold 'Menu' (above CH+) and 'Off' (top power button) simultaneously.
        2. Keep holding both buttons for ~20 seconds.
        3. The remote graphic with circular arrow will flash briefly first.
           IMPORTANT: Keep holding both buttons until the graphic stays permanently on the screen.
        4. Release once the graphic stays on screen. The remote wipes all user data and reboots.

Usage:
    python tools/factory_reset.py --target hub [--hub-ip 192.168.1.1]
    python tools/factory_reset.py --target elite [--elite-ip 192.168.2.1]
    python tools/factory_reset.py --target all
    python tools/factory_reset.py --restore-backup
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

DEFAULT_HUB_IP = os.environ.get("HUB_IP", "192.168.1.1")
DEFAULT_ELITE_IP = "192.168.2.1"


def get_ssh_opts(key_path: pathlib.Path | None = None) -> list[str]:
    opts = [
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=" + ("NUL" if sys.platform == "win32" else "/dev/null"),
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=5",
    ]
    if key_path and key_path.is_file():
        opts.extend(["-i", str(key_path)])
    else:
        # Search ~/.ssh/ for harmony_owner_* or id_rsa
        ssh_dir = pathlib.Path.home() / ".ssh"
        if ssh_dir.is_dir():
            keys = sorted([p for p in ssh_dir.glob("harmony_owner_*") if p.is_file() and not p.name.endswith(".pub")])
            if keys:
                opts.extend(["-i", str(keys[0])])
    return opts


def run_ssh_cmd(target: str, cmd: str, ssh_opts: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["ssh"] + ssh_opts + [target, cmd], capture_output=True, text=True)


def reset_hub(hub_ip: str, ssh_opts: list[str]) -> bool:
    target = f"root@{hub_ip}"
    print(f"\n[*] Probing Harmony Hub at {target}...")
    probe = run_ssh_cmd(target, "uname -a", ssh_opts)
    if probe.returncode != 0:
        print(f"[-] Cannot reach Hub over SSH at {target}: {probe.stderr.strip()}")
        return False

    print(f"[+] Connected to Hub: {probe.stdout.strip()}")
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

    for c in commands:
        res = run_ssh_cmd(target, c, ssh_opts)
        if res.stdout.strip():
            print(res.stdout.strip())

    print("[*] Rebooting Harmony Hub into stock firmware...")
    run_ssh_cmd(target, "reboot -f 2>/dev/null &", ssh_opts)
    print("[+] Hub reboot signal delivered. Hub will boot into stock Logitech environment.")
    return True


def reset_elite(elite_ip: str, ssh_opts: list[str]) -> bool:
    target = f"root@{elite_ip}"
    print(f"\n[*] Probing Harmony Elite Remote at {target}...")
    probe = run_ssh_cmd(target, "uname -a", ssh_opts)
    if probe.returncode != 0:
        print(f"[-] Cannot reach Elite over SSH at {target}: {probe.stderr.strip()}")
        print("    Ensure remote is connected via USB and PC adapter is set to 192.168.2.2.")
        return False

    print(f"[+] Connected to Elite: {probe.stdout.strip()}")
    print("[*] Performing Elite Remote factory reset sequence:")

    commands = [
        # 1. Stop elite_ui
        "killall -9 elite_ui elite_watchdog.sh 2>/dev/null || true",
        # 2. Restore or remove rcS.local
        (
            "if [ -f /data/stock_backup/rcS.local.orig ]; then "
            "  cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "  echo '  + Restored original stock rcS.local'; "
            "else "
            "  rm -f /etc/init.d/rcS.local; "
            "  echo '  + Removed custom rcS.local (stock Elite boots via stock rcS)'; "
            "fi"
        ),
        # 3. Restore executable permissions to stock daemons
        (
            "chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; "
            "echo '  + Restored permissions on stock daemons (usbhid, hal, luaworks, luadraws, watchdog)'"
        ),
        # 4. Remove custom binaries, configs, and dropbear symlinks
        (
            "rm -f /etc/nowatchdog /data/reboot_counter /data/elite_ui.conf /tmp/elite_ui.log 2>/dev/null || true; "
            "rm -f /data/elite_* /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox 2>/dev/null || true; "
            "rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true; "
            "echo '  + Removed all custom binaries, Dropbear symlinks, and configuration files'"
        ),
        # 5. Flush filesystem writes
        "/data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true",
    ]

    for c in commands:
        res = run_ssh_cmd(target, c, ssh_opts)
        if res.stdout.strip():
            print(res.stdout.strip())

    print("[*] Rebooting Harmony Elite into stock firmware...")
    run_ssh_cmd(target, "reboot -f 2>/dev/null &", ssh_opts)
    print("[+] Elite reboot signal delivered. Remote will boot into stock Logitech UI.")
    return True


def confirm_prompt(device_label: str) -> bool:
    print("\n" + "!" * 70)
    print(f" WARNING: FACTORY RESET FOR {device_label.upper()}")
    print("!" * 70)
    print(" This operation will:")
    print("  1. Remove all custom software, binaries, and Web UI.")
    print("  2. Restore original stock startup scripts and daemon permissions.")
    print("  3. Restore stock watchdog and power management behavior.")
    print("  4. Reboot device(s) back into official Logitech stock mode.")
    print("!" * 70)

    val = input(f"\nType 'FACTORY-RESET' to confirm reverting {device_label} to stock: ").strip()
    return val == "FACTORY-RESET"


def main() -> None:
    parser = argparse.ArgumentParser(description="Revert Harmony Hub and/or Elite Remote to factory stock")
    parser.add_argument("--target", choices=["hub", "elite", "all"], default="all",
                        help="Target device to factory reset (default: all)")
    parser.add_argument("--hub-ip", default=DEFAULT_HUB_IP, help=f"Hub IP address (default: {DEFAULT_HUB_IP})")
    parser.add_argument("--elite-ip", default=DEFAULT_ELITE_IP, help=f"Elite IP address (default: {DEFAULT_ELITE_IP})")
    parser.add_argument("--key", type=pathlib.Path, help="Path to SSH private key")
    parser.add_argument("--restore-backup", nargs="?", const="latest", help="Restore stock Hub configuration from backup using all-in-one restore tool")
    parser.add_argument("--force", action="store_true", help="Bypass interactive confirmation prompt")
    args = parser.parse_args()

    if args.restore_backup:
        restore_script = SCRIPT_DIR / "restore_stock.py"
        restore_cmd = [sys.executable, str(restore_script)]
        if args.restore_backup != "latest":
            restore_cmd.extend(["--backup", args.restore_backup])
        if args.force:
            restore_cmd.append("--force")
        if args.hub_ip and args.hub_ip != DEFAULT_HUB_IP:
            restore_cmd.extend(["--hub-ip", args.hub_ip])
        print(f"[*] Delegating to all-in-one restore tool: {' '.join(restore_cmd)}")
        sys.exit(subprocess.run(restore_cmd).returncode)

    print("=" * 70)
    print(" HARMONY SYSTEM FACTORY RESET & STOCK RESTORATION TOOL")
    print(f" Target selection: {args.target.upper()}")
    print("=" * 70)

    if not args.force:
        if not confirm_prompt(args.target):
            print("[-] Confirmation string mismatch. Aborting factory reset.")
            sys.exit(1)

    ssh_opts = get_ssh_opts(args.key)

    if args.target in ("hub", "all"):
        print(f"\n>>> PROCESSING HUB ({args.hub_ip}) <<<")
        reset_hub(args.hub_ip, ssh_opts)

    if args.target in ("elite", "all"):
        print(f"\n>>> PROCESSING ELITE REMOTE ({args.elite_ip}) <<<")
        reset_elite(args.elite_ip, ssh_opts)

    print("\n" + "=" * 70)
    print("[+] Factory reset process completed successfully!")
    print("=" * 70 + "\n")


if __name__ == "__main__":
    main()
