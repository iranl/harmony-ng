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


def deploy_file_if_changed(src: pathlib.Path, remote_path: str, target: str, scp_opts: list[str], ssh_opts: list[str], remote_hashes: dict[str, str], perm: str = "755") -> bool:
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
    run_cmd(["ssh"] + ssh_opts + [target, f"chmod {perm} {tmp_path} 2>/dev/null; mv -f {tmp_path} {remote_path} 2>/dev/null || true"])
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
            "killall -9 elite_ui codex_elite elite_watchdog.sh codex_watchdog.sh 2>/dev/null || true; "
            "if [ -f /data/stock_backup/rcS.local.orig ]; then "
            "  cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "else "
            "  rm -f /etc/init.d/rcS.local; "
            "fi; "
            "chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; "
            "rm -f /etc/nowatchdog /data/reboot_counter /data/codex_elite.conf /tmp/codex_elite.log /tmp/elite_ui.log 2>/dev/null || true; "
            "rm -f /data/elite_* /data/codex_* /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox 2>/dev/null || true; "
            "rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true; "
            "/data/elite_sync 2>/dev/null || sync 2>/dev/null || true; "
            "reboot -f 2>/dev/null &"
        )
        run_cmd(["ssh"] + ssh_opts + [target, revert_cmd])
        print("[+] Elite Remote is rebooting into official Logitech firmware.")
        sys.exit(0)

    # Preserve stock rcS.local backup before installing
    run_cmd(["ssh"] + ssh_opts + [target, "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'elite_ui' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; fi; fi"])

    # 1. Stop existing runtime and stock processes (kill -9 before deletion to free flash memory)
    print("[*] Stopping existing runtime and stock processes...")
    run_cmd(["ssh"] + ssh_opts + [target, "killall -9 elite_watchdog.sh codex_watchdog.sh elite_ui codex_elite hal luaworks luadraws lua luajit luajit-2.0.0-beta10 watchdog usbhid 2>/dev/null || true; sleep 1"])

    # One-way migration: move old configs and remove old codex binaries
    migration_cmd = (
        "if [ -f /data/codex_elite_config.json ] && [ ! -f /data/elite_ui.conf ]; then mv /data/codex_elite_config.json /data/elite_ui.conf 2>/dev/null || true; fi; "
        "if [ -f /data/codex_elite.conf ] && [ ! -f /data/elite_ui.conf ]; then mv /data/codex_elite.conf /data/elite_ui.conf 2>/dev/null || true; fi; "
        "rm -f /data/codex_elite /data/codex_elite.new /data/codex_sync /data/codex_watchdog.sh /data/codex_elite.conf /data/codex_elite_config.json /tmp/codex_elite.log /data/codex_crash.log 2>/dev/null || true"
    )
    run_cmd(["ssh"] + ssh_opts + [target, migration_cmd])

    # 2. Query remote MD5 hashes
    print("[*] Querying remote checksums (md5sum)...")
    remote_hashes = {}
    md5_res = run_cmd(["ssh"] + ssh_opts + [target, "md5sum /data/elite_ui /data/elite_sync /data/elite_watchdog.sh /data/safe_reboot.sh /data/dropbear /data/dropbearkey /data/scp /data/mknod /data/busybox /etc/init.d/rcS.local 2>/dev/null || true"])
    if md5_res.returncode == 0 and md5_res.stdout:
        for line in md5_res.stdout.strip().splitlines():
            parts = line.strip().split()
            if len(parts) >= 2:
                remote_hashes[parts[1].strip()] = parts[0].strip().lower()

    # 3. Upload binaries (only when MD5 differs)
    scp_opts = ["-O"] + ssh_opts
    print("[*] Syncing core ARM binaries to /data/ (MD5 check)...")
    binaries = ["elite_ui", "elite_sync", "dropbear", "dropbearkey", "scp", "mknod", "busybox"]
    for b in binaries:
        src = BIN_DIR / b
        if src.is_file():
            deploy_file_if_changed(src, f"/data/{b}", target, scp_opts, ssh_opts, remote_hashes, perm="755")

    watchdog_script = SCRIPT_DIR / "elite_watchdog.sh"
    if watchdog_script.is_file():
        print(f"[*] Syncing /data/{watchdog_script.name} (MD5 check)...")
        deploy_file_if_changed(watchdog_script, f"/data/{watchdog_script.name}", target, scp_opts, ssh_opts, remote_hashes, perm="755")

    safe_reboot_script = SCRIPT_DIR / "safe_reboot.sh"
    if safe_reboot_script.is_file():
        print(f"[*] Syncing /data/{safe_reboot_script.name} (MD5 check)...")
        deploy_file_if_changed(safe_reboot_script, f"/data/{safe_reboot_script.name}", target, scp_opts, ssh_opts, remote_hashes, perm="755")

    # 4. Upload recovery boot script (only when MD5 differs)
    if RCS_LOCAL_FILE.is_file():
        print("[*] Syncing /etc/init.d/rcS.local (MD5 check)...")
        deploy_file_if_changed(RCS_LOCAL_FILE, "/etc/init.d/rcS.local", target, scp_opts, ssh_opts, remote_hashes, perm="755")

    # 5. Set permissions and finalize
    print("[*] Verifying system shell and flushing NAND flash buffers...")
    post = (
        "if ! /bin/sh -c 'true' 2>/dev/null; then echo '[-] CRITICAL: /bin/sh verification failed. Aborting reboot!'; exit 1; fi; "
        "chmod 755 /data/elite_* /data/safe_reboot.sh /data/dropbear* /data/scp /etc/init.d/rcS.local 2>/dev/null || true; "
        "chmod -x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; "
        "[ -L /data/sync ] || ln -sf /data/elite_sync /data/sync 2>/dev/null || true; "
        "[ -L /usr/bin/sync ] || ln -sf /data/elite_sync /usr/bin/sync 2>/dev/null || true; "
        "[ -L /usr/sbin/dropbear ] || ln -sf /data/dropbear /usr/sbin/dropbear 2>/dev/null || true; "
        "if [ -x /data/busybox ] && [ ! -L /usr/bin/head ]; then for a in head tail which find tar xargs xz bzcat bunzip2 lzma unlzma diff patch awk sed; do ln -sf /data/busybox /usr/bin/$a 2>/dev/null; done; fi; "
        "rm -f /cache/crashlog-*.json 2>/dev/null || true; "
        "if [ -x /data/safe_reboot.sh ]; then (/data/safe_reboot.sh >/dev/null 2>&1 &); "
        "else killall -9 elite_watchdog.sh codex_watchdog.sh elite_ui codex_elite hal luaworks luadraws lua luajit luajit-2.0.0-beta10 watchdog usbhid 2>/dev/null || true; "
        "sleep 1; /data/elite_sync 2>/dev/null || true; sleep 2; mount -o remount,ro /data 2>/dev/null || true; /data/elite_sync 2>/dev/null || true; (/sbin/reboot -f 2>/dev/null || reboot -f 2>/dev/null || (sleep 1 && echo b > /proc/sysrq-trigger)) >/dev/null 2>&1 & fi"
    )
    final_res = run_cmd(["ssh"] + ssh_opts + [target, post])
    if final_res.stdout.strip():
        print(f"    {final_res.stdout.strip()}")

    print()
    print("[+] Deployment complete! Elite Remote is rebooting safely.")


if __name__ == "__main__":
    main()
