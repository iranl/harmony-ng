#!/usr/bin/env python3
"""Deploy the Harmony remote custom runtime over SSH.

Deploys binaries, recovery boot script, and custom UI runtime to Logitech
Harmony remote at 192.168.2.1 (USB Ethernet).

Usage:
    python deploy_remote.py [--ip 192.168.2.1] [--key ~/.ssh/id_rsa]
"""

from __future__ import annotations

import argparse
import hashlib
import io
import pathlib
import shutil
import subprocess
import sys
import tarfile

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
BIN_DIR = SCRIPT_DIR / "bin"
RCS_LOCAL_FILE = SCRIPT_DIR / "rcS.local"
THEMES_DIR = BIN_DIR / "themes"

# Uploaded in this order. busybox goes first: the theme sync unpacks with its tar, which
# stock BusyBox 1.13.4 lacks.
DEPLOY_FILES = [BIN_DIR / b for b in ("busybox", "remote_ui", "remote_sync", "dropbear", "dropbearkey", "scp")] + \
               [SCRIPT_DIR / s for s in ("init.sh", "remote_watchdog.sh", "safe_reboot.sh", "factory_reset.sh")]
REQUIRED_FILES = [BIN_DIR / "remote_ui", BIN_DIR / "remote_sync"]

# Builds before the elite -> remote rename kept /data/elite_* (same steps as "init.sh migrate" and
# remote_migrate.c). Run before the upload: the running binary is then moved to remote_ui, and the
# upload replaces it with the new one. The old names stay as links for an older supervisor that is
# still running until the restart.
MIGRATE_LEGACY_SH = (
    "for p in elite_ui:remote_ui elite_sync:remote_sync; do o=/data/${p%%:*}; n=/data/${p##*:}; "
    "[ -f $o ] && [ ! -L $o ] && mv -f $o $n; done; "
    "for p in elite_watchdog.sh:remote_watchdog.sh elite_settings.conf:remote_settings.conf elite_ui.conf:remote_ui.conf "
    "elite_rf_config.json:remote_rf_config.json elite_activity_usage.conf:remote_activity_usage.conf elite_ui.bak:remote_ui.bak; do "
    "o=/data/${p%%:*}; n=/data/${p##*:}; [ -f $o ] && [ ! -L $o ] && [ ! -f $n ] && mv -f $o $n; done; "
    "[ -d /data/elite_conf_p ] && [ ! -d /data/remote_conf_p ] && mv -f /data/elite_conf_p /data/remote_conf_p; "
    "for p in elite_ui:remote_ui elite_sync:remote_sync elite_watchdog.sh:remote_watchdog.sh elite_settings.conf:remote_settings.conf "
    "elite_rf_config.json:remote_rf_config.json elite_activity_usage.conf:remote_activity_usage.conf; do "
    "o=/data/${p%%:*}; n=${p##*:}; [ -e /data/$n ] && [ ! -e $o ] && ln -s $n $o; done; true"
)
# Commands the deployment runs on the remote, other than through /data/busybox
REMOTE_COMMANDS = ("md5sum", "mkdir", "rm", "mv", "cp", "chmod", "ln", "grep", "touch", "scp")
# The local busybox, run from RAM by the check. BusyBox picks the applet from its own file name,
# so the staged copy must be called busybox: under any other name every applet call fails with
# "applet not found" and the check refuses a fresh install. Same path setup_remote.py stages for
# the backup; the check removes it when done.
CHECK_BUSYBOX = "/tmp/busybox"
FLASH_SLACK_KB = 32  # JFFS2/UBIFS need room beyond each file's own size


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


def kb(n: int) -> int:
    return (n + 1023) // 1024


def theme_dirs() -> list[pathlib.Path]:
    if not THEMES_DIR.is_dir():
        return []
    return sorted(p for p in THEMES_DIR.iterdir() if (p / "theme.json").is_file())


def probe_archive() -> bytes:
    """A one-file archive that a working tar lists as ready_to_go"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        t.addfile(tarfile.TarInfo("ready_to_go"), io.BytesIO(b""))
    return buf.getvalue()


def ready_to_go(target: str, ssh_opts: list[str], scp_opts: list[str], remote_hashes: dict[str, str]) -> bool:
    """Checks, before anything on the remote changes, that the deployment can finish: the local
    files, the remote's commands, a BusyBox with tar for the themes, and the space on /data."""
    problems: list[tuple[str, str]] = []  # what is wrong, what to do about it
    themes = theme_dirs()

    for f in REQUIRED_FILES:
        if not f.is_file():
            problems.append((f"{f} is missing", "Build it with ./build/compile_all.sh remote"))
    if themes and not shutil.which("tar"):
        problems.append(("tar is not installed on this computer; it packs the themes", "Install tar"))

    # The BusyBox whose tar unpacks the themes: the local one when it will replace /data/busybox,
    # run from RAM so the check changes nothing on /data
    local_bb = BIN_DIR / "busybox"
    staged = local_bb.is_file() and remote_hashes.get("/data/busybox") != get_file_md5(local_bb)
    if staged:
        res = run_cmd(["scp"] + scp_opts + [str(local_bb), f"{target}:{CHECK_BUSYBOX}"])
        if res.returncode != 0:
            problems.append((f"Copying {local_bb.name} to the remote's /tmp failed: {res.stderr.strip()}",
                             "Check that the remote has scp (installed with dropbear by remote/setup_remote.py)"))
    bb = CHECK_BUSYBOX if staged else "/data/busybox"

    # tar lists the probe archive from stdin; df's output follows its marker line
    probe = (
        (f"chmod 755 {bb} 2>/dev/null; " if staged else "") +
        "echo arch=$(uname -m); "
        f"for c in {' '.join(REMOTE_COMMANDS)}; do type $c >/dev/null 2>&1 || echo missing=$c; done; "
        f"[ -x {bb} ] && {bb} tar -t 2>/dev/null | grep -q ready_to_go && echo tar=ok; "
        "echo df=; "
        f"df -k /data 2>/dev/null || {bb} df -k /data 2>/dev/null; "
        f"rm -f {CHECK_BUSYBOX}"
    )
    res = subprocess.run(["ssh"] + ssh_opts + [target, probe], input=probe_archive(), capture_output=True)
    facts: dict[str, list[str]] = {}
    out = res.stdout.decode(errors="replace").splitlines()
    for i, line in enumerate(out):
        key, _, value = line.partition("=")
        if key == "df":
            facts["df"] = [" ".join(out[i + 1:])]
            break
        facts.setdefault(key.strip(), []).append(value.strip())

    arch = (facts.get("arch") or [""])[0]
    if res.returncode != 0 and not arch:
        problems.append((f"The check could not run on the remote: {res.stderr.decode(errors='replace').strip()}",
                         "Check the USB connection and SSH key, then run the deployment again"))
    elif not arch.startswith("arm"):
        problems.append((f"{target} is not a Harmony remote (uname -m: {arch or 'unknown'})",
                         "Pass the remote's address with --ip (the remote is 192.168.2.1 over USB)"))
    for c in facts.get("missing", []):
        hint = ("Install dropbear and its scp with remote/setup_remote.py" if c == "scp" else
                "The remote's firmware lacks a command the deployment uses; report this with the remote's model")
        problems.append((f"The remote has no {c} command", hint))
    if themes and "tar" not in facts:
        if local_bb.is_file():
            problems.append((f"{local_bb} has no working tar applet; the themes cannot be unpacked",
                             "Rebuild it with ./build/build_busybox.sh arm"))
        else:
            problems.append(("The remote has no BusyBox with tar (stock BusyBox lacks it), and there is none to install",
                             "Build one with ./build/build_busybox.sh arm, which writes remote/bin/busybox"))

    # Each changed file and each theme is written beside the old copy before it replaces it
    need_kb = 0
    for f in DEPLOY_FILES:
        if f.is_file() and remote_hashes.get(f"/data/{f.name}") != get_file_md5(f):
            need_kb += kb(f.stat().st_size) + FLASH_SLACK_KB
    for theme in themes:
        need_kb += sum(kb(p.stat().st_size) for p in theme.rglob("*") if p.is_file()) + FLASH_SLACK_KB
    # "Filesystem 1K-blocks Used Available Use% Mounted on", then one row, which may wrap
    df = (facts.get("df") or [""])[0].split()
    avail_kb = int(df[-3]) if len(df) >= 10 and df[-3].isdigit() else None
    if avail_kb is None:
        problems.append(("The free space on the remote's /data cannot be measured (stock BusyBox lacks df)",
                         "Build BusyBox with ./build/build_busybox.sh arm, which writes remote/bin/busybox"))
    elif avail_kb < need_kb:
        problems.append((f"The remote's /data has {avail_kb} KB free and the deployment needs {need_kb} KB",
                         "Free space on /data, for example by removing themes from /data/themes that you do not use"))

    print("[*] Ready-to-go check:")
    print(f"    Remote: {arch or 'unknown'}, /data {avail_kb if avail_kb is not None else 'unknown'} KB free, "
          f"{need_kb} KB needed; themes: {len(themes)}, unpacked with {'the new ' if staged else ''}/data/busybox tar")
    if not problems:
        print("[+] Ready to go")
        return True
    print("[-] Not ready to go; nothing on the remote was changed:")
    for what, fix in problems:
        print(f"    - {what}")
        print(f"      Fix: {fix}")
    return False


def main() -> None:
    parser = argparse.ArgumentParser(description="Deploy remote_ui and recovery scripts to a Harmony remote")
    parser.add_argument("--ip", default="192.168.2.1", help="Remote IP address (default 192.168.2.1)")
    parser.add_argument("--user", default="root", help="SSH username (default root)")
    parser.add_argument("--key", type=pathlib.Path, help="SSH private key path")
    parser.add_argument("--factory-reset", action="store_true",
                        help="Revert the Harmony remote to stock firmware (restores stock scripts & permissions)")
    parser.add_argument("--reboot", action="store_true",
                        help="Reboot the remote instead of hot-restarting remote_ui (it also reboots when an old process still holds the watchdog)")
    parser.add_argument("--no-sync", action="store_true",
                        help="Skip forcing full configuration sync from Hub after deployment (default: False)")
    parser.add_argument("--check", action="store_true",
                        help="Run only the ready-to-go check, which changes nothing on the remote")
    args = parser.parse_args()

    target = f"{args.user}@{args.ip}"
    # The keepalive ends a command within 15 s if the remote resets, rather than waiting forever
    ssh_opts = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR",
                "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3"]
    if args.key and args.key.is_file():
        ssh_opts.extend(["-i", str(args.key)])

    print("=" * 60)
    print("  Harmony Remote Custom Runtime Deployment")
    print(f"  Target: {target}")
    if args.factory_reset:
        print("  Mode: FACTORY RESET & STOCK RESTORATION")
    else:
        print("  Mode: Extended BusyBox & Remote Runtime")
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
        print("  WARNING: REVERTING REMOTE TO FACTORY STOCK")
        print("!" * 60)
        conf = input("Type 'RESET' to confirm reverting the remote to stock: ").strip()
        if conf != "RESET":
            print("[-] Confirmation mismatch. Aborting reset.")
            sys.exit(1)
        print("[*] Performing stock revert...")
        revert_cmd = (
            "if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; fi; "
            "killall -9 init.sh remote_ui elite_ui codex_elite remote_watchdog.sh elite_watchdog.sh codex_watchdog.sh 2>/dev/null || true; "
            "if [ -f /data/stock_backup/rcS.local.orig ] && ! grep -q 'usbeth' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'dropbear' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'exit 0' /data/stock_backup/rcS.local.orig 2>/dev/null; then "
            "  cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "else "
            "  printf '#!/bin/sh\\n/usr/bin/flashuboot /lib/firmware/u-boot.bin 2>/dev/null || true\\n' > /etc/init.d/rcS.local; "
            "  chmod 755 /etc/init.d/rcS.local; "
            "fi; "
            "chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true; "
            "rm -f /etc/nowatchdog /etc/tdeenable /data/reboot_counter /data/codex_elite.conf /data/remote_ui.conf /tmp/codex_elite.log /tmp/remote_ui.log /tmp/remote-init.log /tmp/remote_stop /tmp/remote_supervisors.pid /tmp/elite_stop /tmp/elite_supervisors.pid 2>/dev/null || true; "
            "rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true; "
            "rm -f /usr/bin/head /usr/bin/tail /usr/bin/which /usr/bin/find /usr/bin/tar /usr/bin/xargs /usr/bin/xz /usr/bin/bzcat /usr/bin/bunzip2 /usr/bin/lzma /usr/bin/unlzma /usr/bin/diff /usr/bin/patch /usr/bin/awk /usr/bin/sed 2>/dev/null || true; "
            "/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true; sleep 1; "
            "rm -f /data/remote_* /data/elite_* /data/codex_* /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox /data/init.sh /data/safe_reboot.sh /data/factory_reset.sh 2>/dev/null || true; "
            "rm -rf /data/stock_backup /data/assets /data/themes 2>/dev/null || true; "
            "reboot -f 2>/dev/null &"
        )
        run_cmd(["ssh"] + ssh_opts + [target, revert_cmd])
        print("[+] The remote is rebooting into official Logitech firmware.")
        sys.exit(0)

    # Query remote MD5 hashes, then check that the deployment can finish before changing anything
    print("[*] Querying remote checksums (md5sum)...")
    remote_hashes = {}
    md5_res = run_cmd(["ssh"] + ssh_opts + [target, "md5sum /data/remote_ui /data/remote_sync /data/init.sh /data/remote_watchdog.sh /data/safe_reboot.sh /data/factory_reset.sh /data/dropbear /data/dropbearkey /data/scp /data/mknod /data/busybox /etc/init.d/rcS.local 2>/dev/null || true"])
    if md5_res.returncode == 0 and md5_res.stdout:
        for line in md5_res.stdout.strip().splitlines():
            parts = line.strip().split()
            if len(parts) >= 2:
                remote_hashes[parts[1].strip()] = parts[0].strip().lower()

    scp_opts = ["-O"] + ssh_opts
    if not ready_to_go(target, ssh_opts, scp_opts, remote_hashes):
        sys.exit(1)
    if args.check:
        sys.exit(0)

    # Preserve stock rcS.local backup before installing (ensure not corrupted/custom)
    run_cmd(["ssh"] + ssh_opts + [target, "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'elite' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'remote_' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'usbeth' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'dropbear' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'exit 0' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; fi; fi"])

    # 1. remote_ui keeps running while files are uploaded. It feeds the hardware watchdog, whose
    # heartbeat is 10 s and which cannot be stopped once armed, so stopping remote_ui for longer
    # than that resets the remote, possibly while it is writing flash. Each file is uploaded
    # beside the old one and moved into place, which is safe for a running binary, and the
    # remote reboots into the new files at the end.

    # One-way migration: move old configs (Hub config, then settings such as usbeth) and remove old codex binaries
    migration_cmd = (
        MIGRATE_LEGACY_SH + "; "
        "if [ -f /data/codex_elite_config.json ] && [ ! -f /data/remote_rf_config.json ]; then mv /data/codex_elite_config.json /data/remote_rf_config.json 2>/dev/null || true; fi; "
        "if [ -f /data/codex_elite.conf ] && [ ! -f /data/remote_settings.conf ]; then mv /data/codex_elite.conf /data/remote_settings.conf 2>/dev/null || true; fi; "
        "if [ -f /data/remote_ui.conf ] && [ ! -f /data/remote_settings.conf ]; then mv /data/remote_ui.conf /data/remote_settings.conf 2>/dev/null || true; fi; "
        "rm -f /data/codex_elite /data/codex_elite.new /data/codex_sync /data/codex_watchdog.sh /data/codex_elite.conf /data/codex_elite_config.json /data/remote_ui.conf /data/resources/ActivityList.json /tmp/codex_elite.log /data/codex_crash.log 2>/dev/null || true"
    )
    run_cmd(["ssh"] + ssh_opts + [target, migration_cmd])

    # 3. Upload binaries and scripts (only when MD5 differs)
    print("[*] Syncing binaries and scripts to /data/ (MD5 check)...")
    for src in DEPLOY_FILES:
        deploy_file_if_changed(src, f"/data/{src.name}", target, scp_opts, ssh_opts, remote_hashes, perm="755")
        if src.name == "busybox" and theme_dirs():
            # The check found a working tar; stop here, before any other file changes, if the upload lost it
            ok = subprocess.run(["ssh"] + ssh_opts + [target, "/data/busybox tar -t 2>/dev/null | grep -q ready_to_go && echo ok"],
                                input=probe_archive(), capture_output=True)
            if ok.stdout.strip() != b"ok":
                print("[-] /data/busybox has no working tar after the upload; stopping before the other files change.")
                sys.exit(1)

    # Theme packages: each built-in theme replaces its older version; other themes stay. Each is
    # unpacked beside the old one and swapped in, since remote_ui may be drawing from it.
    for theme in theme_dirs():
        print(f"[*] Syncing theme /data/themes/{theme.name}...")
        tar = subprocess.run(["tar", "-c", "-C", str(theme), "."], capture_output=True, check=True).stdout
        d, tmp = f"/data/themes/{theme.name}", f"/data/themes/.{theme.name}.new"  # the theme list skips dot names
        res = subprocess.run(["ssh"] + ssh_opts + [target,
                              f"mkdir -p /data/themes && rm -rf {tmp} && mkdir {tmp} && "
                              f"/data/busybox tar -x -C {tmp} && "
                              f"rm -rf {d} && mv {tmp} {d}"],
                             input=tar, capture_output=True)
        if res.returncode != 0:
            print(f"[-] Failed syncing theme {theme.name}: {res.stderr.decode(errors='replace').strip()}")

    # 4. Upload recovery boot script (only when MD5 differs)
    if RCS_LOCAL_FILE.is_file():
        print("[*] Syncing /etc/init.d/rcS.local (MD5 check)...")
        deploy_file_if_changed(RCS_LOCAL_FILE, "/etc/init.d/rcS.local", target, scp_opts, ssh_opts, remote_hashes, perm="755")

    # 5. Set permissions and finalize
    print("[*] Verifying permissions and flushing NAND flash buffers...")
    post = (
        "sed -i 's/\r$//' /etc/init.d/rcS.local /data/*.sh 2>/dev/null || true; "
        "chmod 755 /data/remote_* /data/init.sh /data/safe_reboot.sh /data/dropbear* /data/scp /etc/init.d/rcS.local 2>/dev/null || true; "
        "chmod -x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; "
        "[ -e /data/sync ] || ln -sf /data/remote_sync /data/sync 2>/dev/null || true; "
        "[ -e /usr/bin/sync ] || ln -sf /data/remote_sync /usr/bin/sync 2>/dev/null || true; "
        "[ -L /usr/sbin/dropbear ] || ln -sf /data/dropbear /usr/sbin/dropbear 2>/dev/null || true; "
        "rm -f /cache/crashlog-*.json 2>/dev/null || true; "
        "/data/remote_sync 2>/dev/null || sync 2>/dev/null || true"
    )
    final_res = run_cmd(["ssh"] + ssh_opts + [target, post])
    if final_res.stdout.strip():
        print(f"    {final_res.stdout.strip()}")

    if not args.no_sync:
        print("[*] Requesting automatic Hub config sync after startup (touch /data/force_sync)...")
        run_cmd(["ssh"] + ssh_opts + [target, "touch /data/force_sync 2>/dev/null || true"])

    # The new files are in place and flushed while remote_ui still feeds the hardware watchdog
    # (DaVinci, 10 s, cannot be stopped once armed). Stop the runtime and start the new one in one
    # short command, so the watchdog goes unfed for a few seconds only. An remote_ui that opened
    # /dev/watchdog without O_CLOEXEC let dropbear inherit it: if anything still holds it after
    # the stop, a restarted remote_ui cannot open it and the board would reset a few seconds
    # later, so reboot cleanly instead.
    stop_cmd = (
        "if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; "
        "else killall -9 remote_watchdog.sh elite_watchdog.sh codex_watchdog.sh remote_ui elite_ui codex_elite hal luaworks luadraws lua luajit luajit-2.0.0-beta10 watchdog usbhid 2>/dev/null || true; fi; "
        "sleep 1; "
    )
    restart = (
        "WDT_HELD=0; for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q watchdog && WDT_HELD=1; done; "
        + ("REBOOT=1; " if args.reboot else "REBOOT=$WDT_HELD; ")
        + "if [ $REBOOT = 1 ]; then echo REBOOTING; "
        "if /bin/sh -c true 2>/dev/null; then (/data/safe_reboot.sh >/dev/null 2>&1 &); else echo '[-] CRITICAL: /bin/sh verification failed. Not rebooting!'; fi; "
        "else echo RESTARTING; rm -f /tmp/remote_stop /tmp/elite_stop 2>/dev/null || true; (/data/init.sh start >/dev/null 2>&1 &); fi"
    )
    print("[*] Restarting the remote runtime (watchdog-aware)...")
    res = run_cmd(["ssh"] + ssh_opts + [target, stop_cmd + restart])
    out = res.stdout.strip()
    for line in out.splitlines():
        if line not in ("REBOOTING", "RESTARTING"):
            print(f"    {line}")
    print()
    if "CRITICAL" in out:
        print(f"[-] Reboot aborted: {(res.stdout + res.stderr).strip()}")
        sys.exit(1)
    if "REBOOTING" in out:
        if args.reboot:
            print("[+] Deployment complete! The remote is rebooting safely.")
        else:
            print("[+] Deployment complete! The watchdog was still held by an old process, so the remote is rebooting safely instead of hot-restarting.")
    elif "RESTARTING" in out:
        print("[+] Deployment complete! remote_ui hot-restarted (no reboot).")
    else:
        print(f"[-] Restart step did not report back (exit {res.returncode}): {res.stderr.strip()}")
        sys.exit(1)


if __name__ == "__main__":
    main()
