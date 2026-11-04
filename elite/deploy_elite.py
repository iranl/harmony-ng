#!/usr/bin/env python3
"""Deploy Harmony Elite Custom Runtime over SSH.

Deploys binaries, recovery boot script, and custom UI runtime to Logitech
Harmony Elite at 192.168.2.1 (USB Ethernet).

Usage:
    python deploy_elite.py [--ip 192.168.2.1] [--key ~/.ssh/id_rsa]
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
BIN_DIR = SCRIPT_DIR / "bin"
RCS_LOCAL_FILE = SCRIPT_DIR / "rcS.local"


def run_cmd(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, capture_output=True, text=True)


def get_file_md5(path: pathlib.Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest().lower()


def stage_file_if_changed(src: pathlib.Path, remote_path: str, target: str, scp_opts: list[str], remote_hashes: dict[str, str], staged: list[tuple[str, str, str]], perm: str = "755") -> bool:
    """Uploads src as <remote_path>.new when its MD5 differs; the swap step moves it into place."""
    if not src.is_file():
        return False
    local_md5 = get_file_md5(src)
    remote_md5 = remote_hashes.get(remote_path)
    if remote_md5 and remote_md5 == local_md5:
        print(f"    -> {src.name} (MD5 match: {local_md5} - skipped)")
        return False
    print(f"    -> Uploading {src.name} (local: {local_md5} != remote: {remote_md5 or 'missing'})...")
    tmp_path = f"{remote_path}.new"
    res = run_cmd(["scp"] + scp_opts + [str(src), f"{target}:{tmp_path}"])
    if res.returncode != 0:
        print(f"[-] Failed uploading {src.name}: {res.stderr.strip()}")
        return False
    staged.append((tmp_path, remote_path, perm))
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Deploy elite_ui and recovery scripts to Harmony Elite")
    parser.add_argument("--ip", default="192.168.2.1", help="Elite IP address (default 192.168.2.1)")
    parser.add_argument("--user", default="root", help="SSH username (default root)")
    parser.add_argument("--key", type=pathlib.Path, help="SSH private key path")
    parser.add_argument("--debug-busybox", "--dev-mode", dest="debug_busybox", action="store_true",
                        help="Install modified extended BusyBox build to /data/busybox (default: False)")
    parser.add_argument("--factory-reset", action="store_true",
                        help="Revert Harmony Elite to stock firmware (restores stock scripts & permissions)")
    parser.add_argument("--reboot", action="store_true",
                        help="Perform hardware reboot after deployment (default: False, hot-restarts services)")
    parser.add_argument("--no-sync", action="store_true",
                        help="Skip forcing full configuration sync from Hub after deployment (default: False)")
    args = parser.parse_args()

    target = f"{args.user}@{args.ip}"
    ssh_opts = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"]
    if args.key and args.key.is_file():
        ssh_opts.extend(["-i", str(args.key)])

    print("=" * 60)
    print("  Harmony Elite Custom Runtime Deployment")
    print(f"  Target: {target}")
    if args.factory_reset:
        print("  Mode: FACTORY RESET & STOCK RESTORATION")
    elif args.debug_busybox:
        print("  Mode: Developer / Debug (Extended BusyBox enabled)")
    else:
        print("  Mode: Standard User (Stock BusyBox; existing installation left untouched)")
    print("=" * 60)

    # 1. Test SSH connectivity
    print("[*] Testing SSH connection...")
    probe = run_cmd(["ssh"] + ssh_opts + [target, "uname -a"])
    if probe.returncode != 0:
        print(f"[-] Cannot connect to {target} over SSH: {probe.stderr.strip()}")
        print("    Verify USB cable is connected and PC interface is set to 192.168.2.2/24.")
        sys.exit(1)
    print(f"[+] Remote system online: {probe.stdout.strip()}")

    if args.factory_reset:
        print("\n" + "!" * 60)
        print("  WARNING: REVERTING ELITE REMOTE TO FACTORY STOCK")
        print("!" * 60)
        conf = input("Type 'RESET' to confirm reverting Elite to stock: ").strip()
        if conf != "RESET":
            print("[-] Confirmation mismatch. Aborting reset.")
            sys.exit(1)
        print("[*] Performing stock revert...")
        revert_cmd = (
            "if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; fi; "
            "killall -9 init.sh elite_ui codex_elite elite_watchdog.sh codex_watchdog.sh 2>/dev/null || true; "
            "if [ -f /data/stock_backup/rcS.local.orig ] && ! grep -q 'usbeth' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'dropbear' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'exit 0' /data/stock_backup/rcS.local.orig 2>/dev/null; then "
            "  cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "else "
            "  printf '#!/bin/sh\\n/usr/bin/flashuboot /lib/firmware/u-boot.bin 2>/dev/null || true\\n' > /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "fi; "
            "chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true; "
            "rm -f /etc/nowatchdog /etc/tdeenable /data/reboot_counter /data/codex_elite.conf /data/elite_ui.conf /tmp/codex_elite.log /tmp/elite_ui.log /tmp/elite-init.log /tmp/elite_stop /tmp/elite_supervisors.pid 2>/dev/null || true; "
            "rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true; "
            "rm -f /usr/bin/head /usr/bin/tail /usr/bin/which /usr/bin/find /usr/bin/tar /usr/bin/xargs /usr/bin/xz /usr/bin/bzcat /usr/bin/bunzip2 /usr/bin/lzma /usr/bin/unlzma /usr/bin/diff /usr/bin/patch /usr/bin/awk /usr/bin/sed 2>/dev/null || true; "
            "/data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true; sleep 1; "
            "rm -f /data/elite_* /data/codex_* /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox /data/init.sh /data/safe_reboot.sh /data/factory_reset.sh 2>/dev/null || true; "
            "rm -rf /data/stock_backup /data/assets 2>/dev/null || true; "
            "reboot -f 2>/dev/null &"
        )
        run_cmd(["ssh"] + ssh_opts + [target, revert_cmd])
        print("[+] Elite Remote is rebooting into official Logitech firmware.")
        sys.exit(0)

    # Preserve stock rcS.local backup before installing (ensure not corrupted/custom)
    run_cmd(["ssh"] + ssh_opts + [target, "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'elite' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'usbeth' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'dropbear' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'exit 0' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; fi; fi"])

    # The remote's hardware watchdog (DaVinci, 10 s heartbeat) is fed by the running elite_ui (or,
    # on stock firmware, by /usr/bin/watchdog). Stopping the runtime first and then uploading for
    # longer than the timeout resets the board in the middle of the flash writes. So the runtime
    # keeps running while every file is uploaded as <path>.new, and stop, swap, flush and restart
    # run afterwards in one short command.
    stop_cmd = (
        "if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; "
        "else killall -9 elite_watchdog.sh codex_watchdog.sh elite_ui codex_elite hal luaworks luadraws lua luajit luajit-2.0.0-beta10 watchdog usbhid 2>/dev/null || true; fi; "
        "sleep 1"
    )

    # One-way migration: move old configs (Hub config, then settings such as usbeth) and remove old codex binaries
    migration_cmd = (
        "if [ -f /data/codex_elite_config.json ] && [ ! -f /data/elite_rf_config.json ]; then mv /data/codex_elite_config.json /data/elite_rf_config.json 2>/dev/null || true; fi; "
        "if [ -f /data/codex_elite.conf ] && [ ! -f /data/elite_settings.conf ]; then mv /data/codex_elite.conf /data/elite_settings.conf 2>/dev/null || true; fi; "
        "if [ -f /data/elite_ui.conf ] && [ ! -f /data/elite_settings.conf ]; then mv /data/elite_ui.conf /data/elite_settings.conf 2>/dev/null || true; fi; "
        "rm -f /data/codex_elite /data/codex_elite.new /data/codex_sync /data/codex_watchdog.sh /data/codex_elite.conf /data/codex_elite_config.json /data/elite_ui.conf /data/resources/ActivityList.json /tmp/codex_elite.log /data/codex_crash.log 2>/dev/null || true"
    )

    # 1. Query remote MD5 hashes
    print("[*] Querying remote checksums (md5sum)...")
    remote_hashes = {}
    md5_res = run_cmd(["ssh"] + ssh_opts + [target, "md5sum /data/elite_ui /data/elite_sync /data/init.sh /data/elite_watchdog.sh /data/safe_reboot.sh /data/factory_reset.sh /data/dropbear /data/dropbearkey /data/scp /data/mknod /data/busybox /etc/init.d/rcS.local 2>/dev/null || true"])
    if md5_res.returncode == 0 and md5_res.stdout:
        for line in md5_res.stdout.strip().splitlines():
            parts = line.strip().split()
            if len(parts) >= 2:
                remote_hashes[parts[1].strip()] = parts[0].strip().lower()

    # 2. Upload changed files as <path>.new while the runtime keeps feeding the watchdog
    scp_opts = ["-O"] + ssh_opts
    staged: list[tuple[str, str, str]] = []
    print("[*] Uploading core ARM binaries to /data/ (MD5 check)...")
    binaries = ["elite_ui", "elite_sync", "dropbear", "dropbearkey", "scp", "busybox"]
    for b in binaries:
        src = BIN_DIR / b
        if src.is_file():
            stage_file_if_changed(src, f"/data/{b}", target, scp_opts, remote_hashes, staged, perm="755")

    for script in ("init.sh", "elite_watchdog.sh", "safe_reboot.sh", "factory_reset.sh"):
        src = SCRIPT_DIR / script
        if src.is_file():
            print(f"[*] Uploading /data/{script} (MD5 check)...")
            stage_file_if_changed(src, f"/data/{script}", target, scp_opts, remote_hashes, staged, perm="755")

    # Recovery boot script (only when MD5 differs)
    if RCS_LOCAL_FILE.is_file():
        print("[*] Uploading /etc/init.d/rcS.local (MD5 check)...")
        stage_file_if_changed(RCS_LOCAL_FILE, "/etc/init.d/rcS.local", target, scp_opts, remote_hashes, staged, perm="755")

    # 3. Stop the runtime, move the uploaded files into place, flush and restart in one command,
    #    so the watchdog goes unfed for a few seconds only
    print(f"[*] Stopping the runtime, installing {len(staged)} uploaded file(s) and flushing NAND flash buffers...")
    swap = "".join(f"chmod {perm} {tmp} 2>/dev/null; mv -f {tmp} {dst} 2>/dev/null || true; " for tmp, dst, perm in staged)
    post = (
        "sed -i 's/\r$//' /etc/init.d/rcS.local /data/*.sh 2>/dev/null || true; "
        "chmod 755 /data/elite_* /data/init.sh /data/safe_reboot.sh /data/dropbear* /data/scp /etc/init.d/rcS.local 2>/dev/null || true; "
        "chmod -x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; "
        "[ -e /data/sync ] || ln -sf /data/elite_sync /data/sync 2>/dev/null || true; "
        "[ -e /usr/bin/sync ] || ln -sf /data/elite_sync /usr/bin/sync 2>/dev/null || true; "
        "[ -L /usr/sbin/dropbear ] || ln -sf /data/dropbear /usr/sbin/dropbear 2>/dev/null || true; "
        "rm -f /cache/crashlog-*.json 2>/dev/null || true; "
    )
    if not args.no_sync:
        print("[*] Requesting automatic Hub config sync after startup (touch /data/force_sync)...")
        post += "touch /data/force_sync 2>/dev/null || true; "
    post += "/data/elite_sync 2>/dev/null || sync 2>/dev/null || true; "
    # An older elite_ui let dropbear inherit /dev/watchdog. If anything still holds it after the
    # stop, a restarted elite_ui cannot open it and the board would reset a few seconds later:
    # reboot cleanly instead.
    restart = (
        "WDT_HELD=0; for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q watchdog && WDT_HELD=1; done; "
        + ("REBOOT=1; " if args.reboot else "REBOOT=$WDT_HELD; ")
        + "if [ $REBOOT = 1 ]; then echo REBOOTING; "
        "if /bin/sh -c true 2>/dev/null; then (/data/safe_reboot.sh >/dev/null 2>&1 &); else echo '[-] CRITICAL: /bin/sh verification failed. Not rebooting!'; fi; "
        "else echo RESTARTING; rm -f /tmp/elite_stop 2>/dev/null || true; (/data/init.sh start >/dev/null 2>&1 &); fi"
    )
    # Flush the uploaded files while the runtime still feeds the watchdog, so the sync after the
    # stop only has the renames left to write
    pre_sync = "/data/elite_sync 2>/dev/null || sync 2>/dev/null || true; "
    final_res = run_cmd(["ssh"] + ssh_opts + [target, pre_sync + stop_cmd + "; " + migration_cmd + "; " + swap + post + restart])
    out = final_res.stdout.strip()
    for line in out.splitlines():
        if line not in ("REBOOTING", "RESTARTING"):
            print(f"    {line}")
    if "REBOOTING" in out:
        if args.reboot:
            print("[+] Deployment complete! Elite Remote is rebooting safely.")
        else:
            print("[+] Deployment complete! The watchdog was still held by an old process, so the Elite Remote is rebooting safely instead of hot-restarting.")
    elif "RESTARTING" in out:
        print("[+] Deployment complete! elite_ui hot-restarted (no reboot).")
    else:
        print(f"[-] Final install step did not report back (exit {final_res.returncode}): {final_res.stderr.strip()}")
    print()
    print("[+] Deployment complete!")


if __name__ == "__main__":
    main()
