#!/usr/bin/env python3
"""Put a Hub on the Linux 6.18 system back on the stock firmware, over SSH, from the backup
tools/install_kernel6.py made at the switch (Windows, macOS, Linux; Python 3 and OpenSSH).

    python tools/rollback_kernel6.py <hub-ip> <backup-dir> [--key KEY] [--factory-fresh] [--yes]

The backup's kernel + kernel2, root, data and cache partitions are written back byte for byte:
the Hub is as it was at the switch (stock firmware, the HarmonyNG and settings of that day;
what changed since is lost). The images go to the Hub's RAM (/tmp) first; the static rescue
hub_util (LINK=static sh build/compile_all.sh util) checks them, makes every flash file system
read-only, writes them and restarts the Hub (hub_util restore). Do not cut the power until the
Hub is back on the stock firmware: a cut write leaves a Hub that needs the serial console.

--factory-fresh: for a backup without data.bin/cache.bin (made by the old shell tool): the data
and cache partitions are erased instead, and the stock firmware starts like a new Hub.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.append(str(HERE))
import ssh_hostkeys  # noqa: E402
from install_kernel6 import Hub, find_key  # noqa: E402

STOCK_PART = 0x500000   # the stock data and cache partitions


def datacache_image(bk: pathlib.Path, factory_fresh: bool) -> bytes:
    """data.bin + cache.bin of the backup, or erased flash for --factory-fresh."""
    data, cache = bk / "data.bin", bk / "cache.bin"
    if data.is_file() and cache.is_file():
        img = data.read_bytes() + cache.read_bytes()
        if len(img) != 2 * STOCK_PART:
            raise SystemExit(f"Error: data.bin and cache.bin must be {STOCK_PART} bytes each")
        return img
    if not factory_fresh:
        raise SystemExit(f"Error: {bk} has no data.bin/cache.bin (a backup of the old shell tool). "
                         "--factory-fresh erases the data instead: the stock firmware then starts like a new Hub.")
    return b"\xff" * (2 * STOCK_PART)


def main() -> None:
    ap = argparse.ArgumentParser(description="Back to the stock firmware from an install_kernel6.py backup")
    ap.add_argument("host")
    ap.add_argument("backup", type=pathlib.Path, help="backups/<hub>-<date> of tools/install_kernel6.py")
    ap.add_argument("--key", "-k", help="SSH private key for root (default: SSH_KEY, ~/.ssh/harmony_owner_*)")
    ap.add_argument("--hub-util", type=pathlib.Path,
                    default=next((p for p in (HERE.parent / "build" / "output" / "kernel6" / "static" / "hub_util",
                                              HERE.parent / "kernel6" / "static" / "hub_util") if p.is_file()),
                                 HERE.parent / "build" / "output" / "kernel6" / "static" / "hub_util"),
                    help="static hub_util (LINK=static sh build/compile_all.sh util; kernel6/static in a release bundle)")
    ap.add_argument("--factory-fresh", action="store_true", help="erase data and cache when the backup has none")
    ap.add_argument("--yes", "-y", action="store_true", help="do not ask")
    ssh_hostkeys.add_reset_option(ap, "the Hub")
    a = ap.parse_args()
    if getattr(a, "reset_host_key", False):
        ssh_hostkeys.reset_host(a.host)

    bk = a.backup
    for f in ("kernel.bin", "kernel2.bin", "root.bin"):
        if not (bk / f).is_file():
            raise SystemExit(f"Error: {bk / f} not found")
    if not a.hub_util.is_file():
        raise SystemExit(f"Error: {a.hub_util} not found")
    root = (bk / "root.bin").read_bytes()
    if root[:4] != b"hsqs":
        raise SystemExit(f"Error: {bk / 'root.bin'} is not a squashfs")
    datacache = datacache_image(bk, a.factory_fresh)

    hub = Hub(a.host, find_key(a.key))
    if "harmony" not in hub.run("uname -r"):
        raise SystemExit(f"Error: {a.host} does not run the 6.18 system")
    if not a.yes:
        print("The Hub writes the stock firmware and restarts; do not cut its power until it is back.")
        if input("Continue? [y/N] ").strip().lower() not in ("y", "yes"):
            raise SystemExit("Nothing changed.")

    print("=== Copying the stock images to the Hub's RAM ===")
    hub.run("rm -rf /tmp/rb && mkdir -p /tmp/rb")
    hub.push(a.hub_util, "/tmp/rb/hub_util")
    hub.push((bk / "kernel.bin").read_bytes() + (bk / "kernel2.bin").read_bytes(), "/tmp/rb/kernels.bin")
    hub.push(root, "/tmp/rb/root.bin")
    hub.push(datacache, "/tmp/rb/datacache.bin")
    imgs = "/tmp/rb/kernels.bin /tmp/rb/root.bin /tmp/rb/datacache.bin"
    hub.run(f"chmod 755 /tmp/rb/hub_util && /tmp/rb/hub_util restore --dry-run {imgs}")

    print("=== Writing the stock system ===")
    # services off (the CC2544 shares the SPI bus with the flash); hub_util restore runs on in
    # its own session: the SSH server dies once its root file system is overwritten
    hub.run("touch /tmp/harmony_stop; for p in $(cat /tmp/harmony_supervisors.pid 2>/dev/null); do kill -9 $p; done; "
            "killall -15 hub_rf 2>/dev/null; sleep 1; killall -9 hub_rf hub_btstack hub_daemon 2>/dev/null; sleep 2; "
            f"setsid /tmp/rb/hub_util restore {imgs} > /tmp/rb/log 2>&1 < /dev/null & sleep 5; cat /tmp/rb/log",
            check=False, timeout=60)

    print("=== Waiting for the stock firmware ===")
    time.sleep(60)
    end = time.time() + 600
    while time.time() < end:
        if hub.reachable():
            rel = hub.run("uname -r", check=False).strip()
            if rel.startswith("2.6.31"):
                print(f"Done: the Hub runs the stock kernel {rel}.")
                return
            if "harmony" in rel:
                raise SystemExit("Error: the Hub still runs the 6.18 system: the restore did not happen. "
                                 "See /tmp/rb/log (gone after a restart) or run it by hand.")
        time.sleep(10)
    print("The Hub does not answer over SSH (the stock firmware of the backup may not run SSH on this "
          "network). Check its light; the serial console shows the boot.")


if __name__ == "__main__":
    main()
