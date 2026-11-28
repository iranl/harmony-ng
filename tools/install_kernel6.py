#!/usr/bin/env python3
"""Install or update the Linux 6.18 Hub system over SSH (Windows, macOS, Linux; needs only
Python 3 and the OpenSSH client).

    python tools/install_kernel6.py <hub-ip> [--key KEY] [--no-deploy] [--yes]

What it does depends on what the Hub runs:

  stock kernel (2.6.31)   the switch. Backs up kernel, kernel2, root, mfg, data and cache to
                          backups/<hub>-<date>/ (tools/rollback_kernel6.py goes back with it),
                          removes the stock-era HarmonyNG binaries (they do not run on 6.18) and,
                          when the data partition is still short of room, the other files a
                          deploy puts back (web UI, icons), stages the root file system image
                          in the data partition, writes the 6.18 kernel and restarts.
  6.18, old flash layout  the switch to one data partition (stock data + cache): stages the
  (a "cache" partition)   image and the boot flags in the data partition, writes the kernel and
                          restarts. Everything else in /cache goes (a deploy puts it back).
  6.18                    an update: kernel, root file system image, rescue scripts.

The kernel's init then does the rest at the restart (build/kernel6/init.c): it erases the old
cache file system once, moves the staged files (cache-import/ at the data partition's top) into
/cache, writes the root file system image and boots it. The tool waits for the Hub and runs
build/deploy_hub.py (HarmonyNG for 6.18) unless --no-deploy. Only this tool switches a Hub to
6.18 or to the new layout: an OTA update never does (payload/scripts/update.sh).

Files, from the build (build/output/kernel6) or a release bundle (kernel6/ next to
deploy_hub.py): harmony-hub-flash.uImage (build/build_kernel6.sh), harmony-hub-rootfs.sqfs
(build/build_rootfs6.sh), static/hub_util (LINK=static sh build/compile_all.sh util), and the
rescue boot's scripts (build/kernel6/*.sh). The CSR8811 PSKEY files and the CC2544 firmware
come from the Hub's own stock root (on a stock Hub) or from --stock-root.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import os
import pathlib
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.append(str(HERE))
import ssh_hostkeys  # noqa: E402

RESCUE_SCRIPTS = ("rc.sh", "udhcpc.sh", "modprobe.sh", "usb_gadget.sh")
# stock firmware the 6.18 system loads from /kernel6/firmware (data partition)
STOCK_FIRMWARE = ("csr8811-a06.psr", "csr8811-a08.psr", "csr8811-a08-pb1.psr", "cc2544.bin")
# boot flags that survive the switch to one data partition (build/kernel6/init.c, rc.sh)
KEEP_FLAGS = ("usb-storage", "serial-console")
# HarmonyNG files a deploy puts back: always (stock-era builds, wrong C library for 6.18) ...
STOCK_ERA = ("/data/harmony/lib", "/data/codex", "/data/test_hub_tx", "/data/rf_pair_test")
# ... and the stock-era binaries in /data/harmony/bin except the SSH server (dropbearmulti and its
# links): the stock Hub needs it until the 6.18 kernel is written; rc.sh links it to the 6.18
# one at the first boot and update.sh cleanup_stale removes /data/dropbearmulti
STOCK_ERA_BIN = ("for f in /data/harmony/bin/*; do case \"${f##*/}\" in dropbear*|scp|ssh) ;; "
                 "*) rm -rf \"$f\" ;; esac; done; true")
# ... and only when the data partition is still short of room
REDEPLOYABLE = ("/data/harmony/www", "/data/harmony/icons.tar.gz", "/data/harmony/remote_ui.ota.gz")

STOCK, OLD_LAYOUT, CURRENT = "stock", "old-layout", "current"


def detect_mode(uname_r: str, proc_mtd: str) -> str:
    """What the Hub runs, from uname -r and /proc/mtd."""
    rel = uname_r.strip()
    if rel.startswith("2.6.31"):
        return STOCK
    if "harmony" not in rel:
        raise SystemExit(f"Error: the Hub runs kernel {rel}: neither the stock 2.6.31 nor the 6.18 system")
    return OLD_LAYOUT if '"cache"' in proc_mtd else CURRENT


def md5_file(path: pathlib.Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_key(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    if os.environ.get("SSH_KEY"):
        return os.environ["SSH_KEY"]
    owners = sorted((p for p in (pathlib.Path.home() / ".ssh").glob("harmony_owner_*")
                     if not p.name.endswith(".pub")), key=lambda p: p.stat().st_mtime, reverse=True)
    return str(owners[0]) if owners else None   # else ssh's own choice


class Hub:
    def __init__(self, host: str, key: str | None):
        self.host = host
        self.key = key

    def _cmd(self, remote: str, timeout: int = 8) -> list[str]:
        cmd = ["ssh", "-o", "BatchMode=yes", "-o", f"ConnectTimeout={timeout}", "-o", "LogLevel=ERROR",
               *ssh_hostkeys.ssh_options(self.host)]
        if self.key:
            cmd += ["-i", self.key]
        return cmd + [f"root@{self.host}", remote]

    def run(self, remote: str, check: bool = True, timeout: int = 600) -> str:
        """Runs a shell command on the Hub (its /bin/sh), returns stdout."""
        r = subprocess.run(self._cmd(remote), capture_output=True, text=True, timeout=timeout)
        if check and r.returncode != 0:
            ssh_hostkeys.report_failure(r.stderr, self.host)
            raise SystemExit(f"Error: on the Hub: {remote.splitlines()[0][:80]}\n{r.stderr.strip()}")
        return r.stdout

    def reachable(self) -> bool:
        try:
            return subprocess.run(self._cmd("true", timeout=5), capture_output=True, timeout=20).returncode == 0
        except subprocess.TimeoutExpired:
            return False

    def push(self, local: pathlib.Path | bytes, remote: str) -> None:
        """Copies a file (or bytes) to the Hub and checks its MD5 there."""
        data = local if isinstance(local, bytes) else local.read_bytes()
        r = subprocess.run(self._cmd(f"cat > '{remote}'"), input=data, capture_output=True, timeout=900)
        if r.returncode != 0:
            raise SystemExit(f"Error: copying to {remote}: {r.stderr.decode(errors='replace').strip()}")
        if self.run(f"md5sum '{remote}'").split()[0] != hashlib.md5(data).hexdigest():
            raise SystemExit(f"Error: {remote} on the Hub does not match the local file")

    def pull(self, remote: str, local: pathlib.Path) -> None:
        with open(local, "wb") as f:
            r = subprocess.run(self._cmd(f"cat '{remote}'"), stdout=f, stderr=subprocess.PIPE, timeout=900)
        if r.returncode != 0:
            raise SystemExit(f"Error: reading {remote}: {r.stderr.decode(errors='replace').strip()}")
        if self.run(f"md5sum '{remote}'").split()[0] != md5_file(local):
            raise SystemExit(f"Error: the copy of {remote} does not match the Hub")

    def mtd(self, name: str) -> str:
        n = self.run(f"grep '\"{name}\"' /proc/mtd | cut -d: -f1").strip()
        if not n:
            raise SystemExit(f"Error: the Hub has no \"{name}\" partition")
        return f"/dev/{n}"


class Installer:
    def __init__(self, hub: Hub, files: pathlib.Path, scripts: pathlib.Path, stock_root: pathlib.Path | None,
                 backups: pathlib.Path):
        self.hub = hub
        self.files = files
        self.scripts = scripts
        self.stock_root = stock_root
        self.backups = backups
        self.flash = files / "harmony-hub-flash.uImage"
        self.rootfs = files / "harmony-hub-rootfs.sqfs"
        self.hub_util = files / "static" / "hub_util"

    def check_files(self) -> None:
        missing = [p for p in (self.flash, self.rootfs, self.hub_util, *(self.scripts / s for s in RESCUE_SCRIPTS))
                   if not p.is_file()]
        if missing:
            raise SystemExit("Error: not found: " + ", ".join(str(p) for p in missing))

    # --- steps shared by the modes ---

    def stop_services(self) -> None:
        # hub_rf first, gracefully: the CC2544 shares the SPI bus with the flash (AGENTS.md)
        self.hub.run("touch /tmp/harmony_stop; for p in $(cat /tmp/harmony_supervisors.pid 2>/dev/null); "
                     "do kill -9 $p; done; killall -15 hub_rf 2>/dev/null; sleep 1; "
                     "killall -9 hub_rf hub_btstack hub_daemon hal luaworks 2>/dev/null; sleep 2; true")

    def free_kb(self, path: str) -> int:
        # the stock BusyBox has no df: HarmonyNG's own in /cache/bin, else (a freshly rooted stock
        # Hub without HarmonyNG) the static hub_util's df
        out = self.hub.run(f"(/cache/bin/busybox df -k '{path}' || df -k '{path}') 2>/dev/null | "
                           "awk 'NR == 2 { print $4 }'").strip()
        if not out.isdigit():
            if self.hub.run("[ -x /tmp/k6_hub_util ] && echo ok; true").strip() != "ok":
                self.hub.push(self.hub_util, "/tmp/k6_hub_util")
                self.hub.run("chmod 755 /tmp/k6_hub_util")
            out = self.hub.run(f"/tmp/k6_hub_util df '{path}' 2>/dev/null; true").strip()
        return int(out) if out.isdigit() else 0

    def make_room(self, data_top: str, need_kb: int, prefix: str, always: tuple[str, ...],
                  if_short: tuple[str, ...]) -> None:
        """Removes always, then what a deploy puts back (if_short) until the data partition has
        need_kb free. prefix: where /data is seen from here ('' through the running root)."""
        if always:
            self.hub.run("rm -rf " + " ".join(f"'{prefix}{p}'" for p in always) + "; true")
        for p in if_short:
            if self.free_kb(data_top) >= need_kb:
                break
            print(f"  removing {p} for room (the deploy puts it back)")
            self.hub.run(f"rm -rf '{prefix}{p}'; true")
        free = self.free_kb(data_top)
        if free < need_kb:
            raise SystemExit(f"Error: the data partition has {free} KB free, the root file system image needs "
                             f"{need_kb} KB. Remove plugins or custom icons (or move them to a USB stick) and "
                             "run this again.")

    def stage(self, kernel6_dir: str) -> None:
        """The root file system image and the rescue scripts to kernel6_dir on the Hub."""
        self.hub.run(f"mkdir -p '{kernel6_dir}'; rm -f '{kernel6_dir}/rootfs.sqfs.failed'")
        for s in RESCUE_SCRIPTS:
            self.hub.push((self.scripts / s).read_bytes().replace(b"\r\n", b"\n"), f"{kernel6_dir}/{s}")
        self.hub.push(self.rootfs, f"{kernel6_dir}/rootfs.sqfs.new")
        self.hub.run(f"chmod 755 '{kernel6_dir}'/*.sh; mv -f '{kernel6_dir}/rootfs.sqfs.new' '{kernel6_dir}/rootfs.sqfs'")

    def firmware_from_stock_root(self, target: str) -> None:
        """PSKEY files and CC2544 firmware from --stock-root, for a Hub whose stock root is gone."""
        self.hub.run(f"mkdir -p '{target}'")
        have = self.hub.run(f"ls '{target}' 2>/dev/null; true").split()
        for name in STOCK_FIRMWARE:
            if name in have:
                continue
            src = self.stock_root / "lib" / "firmware" / name if self.stock_root else None
            if src and src.is_file():
                self.hub.push(src, f"{target}/{name}")
            else:
                print(f"[!] {name} is not on the Hub and no --stock-root has it: "
                      + ("Bluetooth will not start" if name.endswith(".psr") else "/dev/rffw cannot reflash the CC2544"))

    def write_kernel(self) -> None:
        self.hub.push(self.hub_util, "/tmp/k6_hub_util")
        self.hub.push(self.flash, "/tmp/k6_flash.uImage")
        k, k2 = self.hub.mtd("kernel"), self.hub.mtd("kernel2")
        self.hub.run(f"chmod 755 /tmp/k6_hub_util; killall -9 hub_rf hub_btstack 2>/dev/null; sleep 1; "
                     f"/tmp/k6_hub_util flash-image /tmp/k6_flash.uImage {k} {k2}")

    def restart(self) -> None:
        self.hub.run("(/data/harmony/bin/hub_sync || /tmp/k6_hub_util sync || sync) 2>/dev/null; sleep 2; "
                     "for m in $(awk '$3 == \"jffs2\" { print $2 }' /proc/mounts); do mount -o remount,ro $m 2>/dev/null; done; "
                     "/bin/sh -c true && (sleep 1; echo b > /proc/sysrq-trigger 2>/dev/null; reboot -f) >/dev/null 2>&1 &",
                     check=False, timeout=30)

    def wait_back(self) -> None:
        print("=== Waiting for the Hub (the first boot writes the root file system: a few minutes) ===")
        time.sleep(20)
        end = time.time() + 600
        while time.time() < end:
            if self.hub.reachable():
                rel = self.hub.run("uname -r").strip()
                mtd = self.hub.run("cat /proc/mtd")
                if detect_mode(rel, mtd) == CURRENT and self.hub.run("grep -c '^overlay / overlay' /proc/mounts; true").strip() == "1":
                    print(f"  back: {rel}, one data partition, root file system boot")
                    return
                raise SystemExit(f"Error: the Hub is back but not as expected ({rel}; root file system boot: no). "
                                 "Over SSH: cat /tmp/rc.log, ls /cache/kernel6")
            time.sleep(10)
        raise SystemExit("Error: the Hub did not come back within 10 minutes. Check the light; the serial "
                         "console shows the boot (docs/OFFLINE_UNBRICK_AND_ROOT_GUIDE.md).")

    # --- the modes ---

    def from_stock(self) -> None:
        h = self.hub
        if h.run("ls /lib/firmware/csr8811-a06.psr /lib/firmware/csr8811-a08.psr "
                 "/lib/firmware/csr8811-a08-pb1.psr >/dev/null 2>&1 && echo ok; true").strip() != "ok":
            raise SystemExit("Error: the stock PSKEY files are missing from /lib/firmware")
        bk = self.backups / f"{h.host}-{datetime.datetime.now():%Y%m%d-%H%M%S}"
        print(f"=== Backing up to {bk} ===")
        bk.mkdir(parents=True)
        for part in ("kernel", "kernel2", "root", "mfg", "data", "cache"):
            h.pull(h.mtd(part), bk / f"{part}.bin")
            print(f"  {part}: {(bk / f'{part}.bin').stat().st_size} bytes")
        print("=== Stopping HarmonyNG, making room in the data partition ===")
        self.stop_services()
        h.run(STOCK_ERA_BIN)
        # the stock union root writes to the data partition's top: /kernel6, /cache-import
        need = self.rootfs.stat().st_size // 1024 + 256
        self.make_room("/data", need, "", STOCK_ERA, REDEPLOYABLE)
        print("=== Staging the 6.18 system ===")
        self.stage("/cache-import/kernel6")
        h.run("mkdir -p /kernel6/firmware && cp " + " ".join(f"/lib/firmware/{n}" for n in STOCK_FIRMWARE[:3])
              + " /kernel6/firmware/ && { cp /lib/firmware/cc2544.bin /kernel6/firmware/ 2>/dev/null; true; }")
        print("=== Writing the 6.18 kernel ===")
        self.write_kernel()
        print("=== Restarting ===")
        self.restart()
        print(f"Backup for tools/rollback_kernel6.py: {bk}")

    def from_old_layout(self) -> None:
        h = self.hub
        mounts = h.run("cat /proc/mounts")
        if " /overlay jffs2 " in mounts:
            top, prefix = "/overlay", ""   # the running overlay root shows /data
        elif " /mnt/data jffs2 " in mounts:
            top = "/mnt/data"
            prefix = "/mnt/data/upper" if h.run("[ -d /mnt/data/upper ] && echo y; true").strip() else "/mnt/data"
        else:   # data and cache on a USB stick: the flash data partition is not mounted
            h.run("mkdir -p /tmp/k6data && mount -t jffs2 mtd:data /tmp/k6data")
            top, prefix = "/tmp/k6data", "/tmp/k6data/upper"
        print("=== Stopping HarmonyNG ===")
        self.stop_services()
        need = self.rootfs.stat().st_size // 1024 + 256
        # the 6.18 binaries in /data/harmony/bin work: they go only when room is short
        self.make_room(top, need, prefix, (), REDEPLOYABLE + ("/data/harmony/bin",))
        print("=== Staging the 6.18 system and the boot flags ===")
        self.stage(f"{top}/cache-import/kernel6")
        for flag in KEEP_FLAGS:
            h.run(f"[ -f /cache/kernel6/{flag} ] && cp /cache/kernel6/{flag} '{top}/cache-import/kernel6/'; true")
        self.firmware_from_stock_root(f"{prefix}/kernel6/firmware")
        print("=== Writing the 6.18 kernel (one data partition) ===")
        self.write_kernel()
        print("=== Restarting ===")
        self.restart()

    def update(self) -> None:
        print("=== Stopping HarmonyNG ===")
        self.stop_services()
        need = self.rootfs.stat().st_size // 1024 + 128
        if self.free_kb("/cache/kernel6") < need:
            raise SystemExit(f"Error: /cache has less than {need} KB free for the root file system image")
        print("=== Staging the root file system and the rescue scripts ===")
        self.stage("/cache/kernel6")
        self.firmware_from_stock_root("/kernel6/firmware")
        print("=== Writing the kernel ===")
        self.write_kernel()
        print("=== Restarting ===")
        self.restart()


def find_deploy_tool() -> pathlib.Path | None:
    for p in (HERE.parent / "build" / "deploy_hub.py", HERE.parent / "deploy_hub.py"):
        if p.is_file():
            return p
    return None


def main() -> None:
    ap = argparse.ArgumentParser(description="Install or update the Linux 6.18 Hub system over SSH")
    ap.add_argument("host", help="Hub IP or host name")
    ap.add_argument("--key", "-k", help="SSH private key for root (default: SSH_KEY, ~/.ssh/harmony_owner_*)")
    ap.add_argument("--files", type=pathlib.Path,
                    help="folder with harmony-hub-flash.uImage, harmony-hub-rootfs.sqfs, static/hub_util "
                         "(default: build/output/kernel6, or kernel6/ of a release bundle)")
    ap.add_argument("--stock-root", type=pathlib.Path,
                    help="extracted stock root file system, for the PSKEY files and cc2544.bin on a Hub "
                         "that has none (default: background/hub_firmware_extracted/squashfs-root)")
    ap.add_argument("--no-deploy", action="store_true", help="do not run build/deploy_hub.py afterwards")
    ap.add_argument("--yes", "-y", action="store_true", help="do not ask before the switch")
    ssh_hostkeys.add_reset_option(ap, "the Hub")
    a = ap.parse_args()
    if getattr(a, "reset_host_key", False):
        ssh_hostkeys.reset_host(a.host)

    repo = HERE.parent
    files = a.files or next((p for p in (repo / "build" / "output" / "kernel6", repo / "kernel6") if p.is_dir()),
                            repo / "build" / "output" / "kernel6")
    scripts = repo / "build" / "kernel6" if (repo / "build" / "kernel6").is_dir() else files
    stock_root = a.stock_root or repo / "background" / "hub_firmware_extracted" / "squashfs-root"
    inst = Installer(Hub(a.host, find_key(a.key)), files, scripts, stock_root if stock_root.is_dir() else None,
                     repo / "backups")
    inst.check_files()

    h = inst.hub
    if not h.reachable():
        raise SystemExit(f"Error: no SSH login as root on {a.host}")
    mode = detect_mode(h.run("uname -r"), h.run("cat /proc/mtd"))
    print(f"=== {a.host}: " + {STOCK: "stock kernel: switch to the Linux 6.18 system",
                                OLD_LAYOUT: "6.18 system, old flash layout: switch to one data partition",
                                CURRENT: "6.18 system: update"}[mode] + " ===")
    if mode != CURRENT and not a.yes:
        print("The Hub restarts and writes its flash; do not cut its power until it is back (a few minutes).")
        if mode == OLD_LAYOUT:
            print("Everything in /cache except the boot flags goes (the deploy afterwards puts HarmonyNG's files back).")
        if input("Continue? [y/N] ").strip().lower() not in ("y", "yes"):
            raise SystemExit("Nothing changed.")
    {STOCK: inst.from_stock, OLD_LAYOUT: inst.from_old_layout, CURRENT: inst.update}[mode]()
    inst.wait_back()
    deploy = find_deploy_tool()
    if a.no_deploy or not deploy:
        print("Done." + ("" if a.no_deploy else " Run build/deploy_hub.py to install HarmonyNG."))
        return
    print("=== HarmonyNG (build/deploy_hub.py) ===")
    cmd = [sys.executable, str(deploy), a.host] + (["--key", inst.hub.key] if inst.hub.key else [])
    sys.exit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
