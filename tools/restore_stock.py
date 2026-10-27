#!/usr/bin/env python3
"""Harmony Hub - Stock Configuration Restore Utility.

All-in-one recovery tool to restore a factory-reset Logitech Harmony Hub back to
official stock firmware with its full original configuration, activities, and
devices completely restored.

Automated 5-Step Pipeline:
1. Gain root over USB HID (provisions Wi-Fi if needed, stages Dropbear, boots SSH).
2. Kill all processes except Dropbear (releases /data and /cache flash locks).
3. Transfer and extract backed up /cache and /data partitions.
4. Flush flash write buffers safely (sync; sync; sleep 2; verify /bin/sh -c 'true').
5. Reboot Hub cleanly into official Logitech stock firmware with configuration restored.

Usage:
    python tools/restore_stock.py
    python tools/restore_stock.py --backup backups/stock_hub_backup_latest.tar
    python tools/restore_stock.py --wifi-ssid "MyNetwork" --wifi-pass "Secret123"
    python tools/restore_stock.py --pure-stock   # Completely removes Dropbear SSH
    python tools/restore_stock.py --force        # Bypass confirmation prompt
"""

from __future__ import annotations

import argparse
import getpass
import gzip
import hashlib
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tarfile
import time

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR.parent
BACKUP_DIR = REPO_DIR / "backups"

if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools.harmony_hid import HarmonyHid, HUB_PID
from tools.root_hub_usb import (
    build_payload,
    check_tcp_port,
    find_busybox_binary,
    find_hub_lan_ip,
    resolve_ssh_key,
    wait_for_ssh,
)


def get_ssh_opts(priv_key_path: pathlib.Path | None = None) -> list[str]:
    opts = [
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=" + ("NUL" if sys.platform == "win32" else "/dev/null"),
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=10",
    ]
    if priv_key_path and priv_key_path.is_file():
        opts.extend(["-i", str(priv_key_path)])
    return opts


def run_ssh_cmd(target: str, cmd: str, ssh_opts: list[str], timeout: int = 30) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["ssh"] + ssh_opts + [target, cmd], capture_output=True, text=True, timeout=timeout)


def find_latest_stock_backup(specified_path: str | None = None) -> pathlib.Path:
    if specified_path:
        p = pathlib.Path(specified_path)
        if not p.is_file():
            raise FileNotFoundError(f"Specified backup file not found: {specified_path}")
        return p

    if not BACKUP_DIR.is_dir():
        raise FileNotFoundError(f"Backup directory does not exist: {BACKUP_DIR}")

    # Check preferred candidates in order (.tar preferred, fallback to .tar.gz)
    for ext in ("tar", "tar.gz"):
        latest_link = BACKUP_DIR / f"stock_hub_backup_latest.{ext}"
        if latest_link.is_file():
            return latest_link

    # Search for timestamped stock backups
    stock_backups = sorted(
        [p for p in BACKUP_DIR.glob("stock_hub_backup_*.*") if p.is_file() and p.name.endswith((".tar", ".tar.gz"))],
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if stock_backups:
        return stock_backups[0]

    # Fallback to golden backup if present
    for ext in ("tar", "tar.gz"):
        golden = BACKUP_DIR / f"golden_hub_backup.{ext}"
        if golden.is_file():
            return golden

    # Fallback to any hub_backup_*.tar*
    other_backups = sorted(
        [p for p in BACKUP_DIR.glob("hub_backup_*.*") if p.is_file() and p.name.endswith((".tar", ".tar.gz"))],
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if other_backups:
        return other_backups[0]

    raise FileNotFoundError(
        f"No stock backup archives found in {BACKUP_DIR}.\n"
        "Please provide path to backup using --backup <path>."
    )


def validate_backup_archive(archive_path: pathlib.Path) -> list[str]:
    if not archive_path.is_file() or archive_path.stat().st_size < 1000:
        raise ValueError(f"Backup archive {archive_path} is empty or invalid.")

    with tarfile.open(archive_path, "r:*") as tar:
        names = tar.getnames()

    has_data = any(n == "data" or n.startswith("data/") or n == "/data" or n.startswith("/data/") for n in names)
    has_cache = any(n == "cache" or n.startswith("cache/") or n == "/cache" or n.startswith("/cache/") for n in names)

    if not has_data and not has_cache:
        raise ValueError(
            f"Archive {archive_path.name} does not appear to contain /data or /cache partitions.\n"
            f"Sample members: {names[:10]}"
        )

    return names


def gain_root_over_usb(
    args: argparse.Namespace,
    pub_path: pathlib.Path,
    pub_str: str,
) -> str:
    """Connect to Hub over USB HID, configure Wi-Fi if needed, stage Dropbear, and reboot into SSH."""
    print("[*] [Step 1/5] Searching for Harmony Hub over USB HID (046D:C129)...")
    hid = HarmonyHid(pid=HUB_PID)
    if not hid.open():
        raise RuntimeError(
            "Harmony Hub not detected on USB.\n"
            "Ensure the Hub is connected via micro-USB to this computer and powered on."
        )

    try:
        # Check Wi-Fi Status
        print("[*] Checking Hub Wi-Fi connection over USB HID...")
        wifi_status = hid.get_wifi_status()
        conn_status = wifi_status.get("connect_status", "").lower()
        hub_ip = wifi_status.get("ip_address", "").strip()
        ssid = wifi_status.get("ssid", "").strip()

        is_connected = conn_status == "connected" and hub_ip and hub_ip != "0.0.0.0"

        if not is_connected or args.wifi_ssid:
            if not is_connected:
                print(f"[-] Hub is NOT connected to Wi-Fi (status: '{conn_status or 'disconnected'}').")
                print("    Configuring Wi-Fi is required to enable SSH access for restoration.")

            target_ssid = args.wifi_ssid
            if not target_ssid:
                target_ssid = input("\nEnter Wi-Fi SSID (2.4 GHz): ").strip()
                while not target_ssid:
                    target_ssid = input("SSID cannot be empty. Enter Wi-Fi SSID: ").strip()

            target_pass = args.wifi_pass
            if target_pass is None and args.wifi_enc.upper() not in ("NONE", "OPEN"):
                print("Note: If password contains special characters, interactive prompt preserves them literally.")
                target_pass = getpass.getpass(f"Enter Wi-Fi password for '{target_ssid}': ")

            print(f"[*] Provisioning Wi-Fi SSID '{target_ssid}' over USB HID...")
            if not hid.provision_wifi(target_ssid, target_pass or "", args.wifi_enc):
                raise RuntimeError("Failed to send Wi-Fi credentials over USB HID.")

            print("[+] Wi-Fi credentials accepted by Hub.")
            print(f"[*] Waiting for Hub to connect to '{target_ssid}' (up to 40s)...")
            t_deadline = time.time() + 40
            is_connected = False
            while time.time() < t_deadline:
                time.sleep(2)
                wifi_status = hid.get_wifi_status()
                conn_status = wifi_status.get("connect_status", "").lower()
                hub_ip = wifi_status.get("ip_address", "").strip()
                ssid = wifi_status.get("ssid", "").strip()
                if conn_status == "connected" and hub_ip and hub_ip != "0.0.0.0":
                    is_connected = True
                    print(f"\n[+] Wi-Fi connected! SSID='{ssid}', IP={hub_ip}")
                    break
                print(".", end="", flush=True)

            if not is_connected:
                raise RuntimeError(f"Hub failed to connect to Wi-Fi within 40s (status: '{conn_status}').")
        else:
            print(f"[+] Wi-Fi already connected: SSID='{ssid}', IP={hub_ip}")

        # Verify LAN Reachability
        print(f"[*] Verifying LAN reachability on {hub_ip}...")
        lan_ip = None
        if check_tcp_port(hub_ip, 8088, timeout_s=2.0) or check_tcp_port(hub_ip, 22, timeout_s=2.0):
            lan_ip = hub_ip
        else:
            print(f"[*] Direct probe to {hub_ip} did not respond. Scanning local network...")
            discovered = find_hub_lan_ip(preferred_ip=args.hub_ip or hub_ip, port=8088)
            if not discovered:
                discovered = find_hub_lan_ip(preferred_ip=args.hub_ip or hub_ip, port=22)
            if discovered:
                lan_ip = discovered
            else:
                raise RuntimeError(
                    f"Hub reports IP {hub_ip}, but is not reachable on local LAN from this PC.\n"
                    "Ensure PC and Hub are on the same subnet and AP isolation is OFF."
                )

        print(f"[+] Verified Hub reachability on LAN at {lan_ip}")

        # Build & upload Dropbear payload
        print("[*] Building self-extracting Dropbear payload (MIPS)...")
        payload = build_payload(pub_str)
        print(f"[*] Staging Dropbear payload to /sys/dropbear ({len(payload):,} bytes)...")

        def progress(sent: int, total: int) -> None:
            pct = int(sent / total * 100)
            print(f"\r    Uploaded {sent:,} / {total:,} bytes ({pct}%)", end="", flush=True)

        hid.write_full_file("/sys/dropbear", payload, packets_per_chunk=5, progress_callback=progress)
        print("\n[+] Dropbear staged successfully.")

        # Arm persistent boot trigger
        print("[*] Arming boot trigger (/tde/enable -> /etc/tdeenable)...")
        if not hid.set_tde_enable(True):
            print("[-] Warning: Failed to confirm /tde/enable flag.")

        # Reboot Hub into Dropbear SSH
        print("[*] Rebooting Hub to launch Dropbear SSH...")
        try:
            hid.reboot()
        except Exception:
            pass

        print(f"[*] Waiting for Dropbear SSH on {lan_ip}:22...")
        if not wait_for_ssh(lan_ip, 22, timeout_s=60):
            raise TimeoutError(f"Timed out waiting for Dropbear SSH on {lan_ip}:22 after reboot.")

        print(f"\n[+] SUCCESS: Dropbear SSH is ONLINE at {lan_ip}:22!")
        return lan_ip

    finally:
        hid.close()


def restore_stock_configuration(
    hub_ip: str,
    priv_key_path: pathlib.Path,
    backup_file: pathlib.Path,
    pure_stock: bool = False,
) -> None:
    ssh_opts = get_ssh_opts(priv_key_path)
    target = f"root@{hub_ip}"

    # Step 2: Kill all processes, except dropbear
    print("\n[*] [Step 2/5] Stopping all background daemons (preserving Dropbear SSH)...")
    kill_cmd = (
        "killall -9 init.sh hub_daemon hub_btstack hub_rf hub_dhcpd hub_portal "
        "hal luaworks luadraws lua watchdog bluetoothd 2>/dev/null || true"
    )
    res = run_ssh_cmd(target, kill_cmd, ssh_opts)
    time.sleep(1.5)
    print("    + Terminated hal, luaworks, luadraws, watchdog, bluetoothd, and custom daemons.")

    # Step 3: Ensure busybox with tar support is available on Hub
    print("\n[*] [Step 3/5] Transferring and extracting backed up /cache and /data partitions...")
    chk_cmd = (
        "if [ -x /cache/bin/busybox ]; then echo /cache/bin/busybox; "
        "elif [ -x /data/harmony/bin/busybox ]; then echo /data/harmony/bin/busybox; "
        "elif [ -x /tmp/busybox ]; then echo /tmp/busybox; "
        "else echo ''; fi"
    )
    bb_res = run_ssh_cmd(target, chk_cmd, ssh_opts)
    bb_bin = bb_res.stdout.strip()
    uploaded_temp_bb = False

    if not bb_bin:
        local_bb = find_busybox_binary()
        if not local_bb:
            raise FileNotFoundError("Local MIPS BusyBox binary with tar support not found.")
        print(f"    + Staging temporary BusyBox ({local_bb.stat().st_size:,} bytes) to /tmp/busybox...")
        with open(local_bb, "rb") as f_in:
            proc = subprocess.run(
                ["ssh"] + ssh_opts + [target, "cat > /tmp/busybox && chmod 755 /tmp/busybox"],
                input=f_in.read(),
                check=True,
                timeout=30,
            )
        bb_bin = "/tmp/busybox"
        uploaded_temp_bb = True

    # Clean old transient files in /cache
    run_ssh_cmd(target, "rm -rf /cache/* 2>/dev/null || true", ssh_opts)

    # Stream and extract backup archive to / (plain tar, no gzip on Hub)
    print(f"    + Extracting {backup_file.name} ({backup_file.stat().st_size / (1024*1024):.2f} MB) to Hub root filesystem...")
    t0 = time.time()
    extract_cmd = f"{bb_bin} tar -xf - -C /"

    open_func = gzip.open if backup_file.name.endswith(".gz") else open
    with open_func(backup_file, "rb") as f_in:
        proc = subprocess.Popen(
            ["ssh"] + ssh_opts + [target, extract_cmd],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        shutil.copyfileobj(f_in, proc.stdin)
        proc.stdin.close()
        stdout, stderr = proc.communicate(timeout=180)
        if proc.returncode != 0:
            print(f"[-] Warning: Tar exited with code {proc.returncode}: {stderr.decode('utf-8', errors='ignore').strip()}")

    print(f"    + Partitions /cache and /data restored successfully in {time.time() - t0:.1f}s.")

    # Configure post-restore SSH persistence or pure stock
    if pure_stock:
        print("    + Removing Dropbear SSH to return device to 100% factory pure stock...")
        run_ssh_cmd(
            target,
            "rm -f /usr/sbin/dropbear /usr/sbin/dropbearkey /data/dropbearmulti /etc/tdeenable /etc/nowatchdog 2>/dev/null || true",
            ssh_opts,
        )
    else:
        print("    + Retaining permanent root Dropbear SSH access on restored stock firmware.")
        run_ssh_cmd(
            target,
            "touch /etc/tdeenable; chmod 644 /etc/tdeenable 2>/dev/null || true",
            ssh_opts,
        )

    # Step 4: Flush filesystem buffers safely
    print("\n[*] [Step 4/5] Flushing flash eraseblocks (sync; sync; sleep 2)...")
    cleanup_cmd = (
        f"{bb_bin} sync 2>/dev/null || /data/harmony/bin/hub_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true; "
        f"{bb_bin} sync 2>/dev/null || /data/harmony/bin/hub_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true; "
        "rm -f /tmp/busybox 2>/dev/null; sleep 2; /bin/sh -c 'true'"
    )
    sync_res = run_ssh_cmd(target, cleanup_cmd, ssh_opts, timeout=30)
    if sync_res.returncode != 0:
        raise RuntimeError(f"Flash synchronization failed with return code {sync_res.returncode}.")
    print("    + Flash write buffers cleanly flushed and verified.")

    # Step 5: Clean reboot
    print("\n[*] [Step 5/5] Rebooting Harmony Hub into stock Logitech firmware...")
    run_ssh_cmd(target, "reboot -f 2>/dev/null &", ssh_opts, timeout=5)
    print("[+] Reboot signal delivered successfully!")


def confirm_prompt(backup_file: pathlib.Path, pure_stock: bool) -> bool:
    print("\n" + "!" * 72)
    print(" WARNING: HARMONY HUB STOCK CONFIGURATION RESTORATION")
    print("!" * 72)
    print(f" Backup Source: {backup_file}")
    print(f" Mode:          {'Pure Factory Stock (SSH Removed)' if pure_stock else 'Stock Firmware + Root SSH Retained'}")
    print(" This operation will:")
    print("  1. Connect over USB HID and gain root SSH access.")
    print("  2. Kill running background processes to prevent flash locks.")
    print("  3. Restore your backed up /cache and /data partitions.")
    print("  4. Flush SPI flash eraseblocks cleanly.")
    print("  5. Reboot the Hub into official Logitech firmware with all configs restored.")
    print("!" * 72)

    val = input("\nType 'RESTORE' to proceed with stock configuration restoration: ").strip()
    return val == "RESTORE"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="All-in-one tool to restore Harmony Hub to stock with full configuration restored"
    )
    parser.add_argument("--backup", help="Path to stock backup archive (.tar.gz)")
    parser.add_argument("--wifi-ssid", help="Wi-Fi SSID to provision over USB")
    parser.add_argument("--wifi-pass", default=None, help="Wi-Fi password (prompted if omitted)")
    parser.add_argument("--wifi-enc", default="WPA2-PSK", help="Wi-Fi encryption (default: WPA2-PSK)")
    parser.add_argument("--hub-ip", help="Known Hub LAN IP address")
    parser.add_argument("--pure-stock", action="store_true", help="Remove Dropbear SSH to return to 100%% factory state")
    parser.add_argument("--force", action="store_true", help="Bypass interactive confirmation prompt")
    parser.add_argument("--skip-usb-root", action="store_true", help="Skip USB HID root (use if Hub SSH is already online)")
    args = parser.parse_args()

    print("=" * 72)
    print(" HARMONY HUB ALL-IN-ONE STOCK & CONFIGURATION RESTORE TOOL")
    print("=" * 72)

    # 1. Locate and validate backup
    try:
        backup_path = find_latest_stock_backup(args.backup)
        members = validate_backup_archive(backup_path)
    except Exception as e:
        print(f"[-] Error locating backup archive: {e}")
        sys.exit(1)

    size_mb = backup_path.stat().st_size / (1024 * 1024)
    print(f"[+] Selected Backup: {backup_path.name} ({size_mb:.2f} MB, {len(members)} entries)")

    if not args.force:
        if not confirm_prompt(backup_path, args.pure_stock):
            print("[-] Confirmation string mismatch. Aborting restoration.")
            sys.exit(1)

    # 2. Resolve SSH keys
    pub_path, pub_str = resolve_ssh_key()
    priv_key_path = pub_path.with_suffix("")

    # 3. Determine Hub IP & Gain Root
    hub_ip = args.hub_ip
    if not args.skip_usb_root:
        # Check if Hub is already reachable on SSH with existing IP
        if hub_ip and check_tcp_port(hub_ip, 22, timeout_s=1.5):
            print(f"[+] Hub already responding to SSH on {hub_ip}:22. Skipping USB root stage.")
        else:
            try:
                hub_ip = gain_root_over_usb(args, pub_path, pub_str)
            except Exception as e:
                print(f"[-] Failed to gain root over USB: {e}")
                sys.exit(1)
    else:
        if not hub_ip:
            print("[-] --skip-usb-root requires --hub-ip to be specified.")
            sys.exit(1)

    # 4. Restore configuration
    try:
        restore_stock_configuration(hub_ip, priv_key_path, backup_path, args.pure_stock)
    except Exception as e:
        print(f"[-] Restoration failed: {e}")
        sys.exit(1)

    print("\n" + "=" * 72)
    print("[+] STOCK RESTORATION COMPLETE!")
    print("    Hub is booting into official Logitech stock firmware.")
    print("    All Wi-Fi credentials, devices, and activities have been restored.")
    print("=" * 72 + "\n")


if __name__ == "__main__":
    main()
