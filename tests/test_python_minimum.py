#!/usr/bin/env python3
"""
The host tools run on the user's own Python. Each one must stop at once with a clear message on a
Python older than 3.11 (the minimum in docs/FIRST_INSTALL_*.md), and must not stop on a supported one.
Runs every tool as __main__ with --help: once with sys.version_info set to 3.10, once unchanged.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = [
    "build/deploy_hub.py",
    "remote/setup_remote.py",
    "remote/deploy_remote.py",
    "tools/restore_stock.py",
    "tools/harmony_firmware.py",
    "tools/root_hub_usb.py",
    "tools/migrate_hub_configs.py",
    "tools/backup_hub.py",
    "tools/factory_reset.py",
]
RUN = (
    "import runpy, sys\n"
    "if sys.argv[1] == 'old': sys.version_info = (3, 10, 12, 'final', 0)\n"
    "tool = sys.argv[2]\n"
    "sys.argv = [tool, '--help']\n"
    "runpy.run_path(tool, run_name='__main__')\n"
)
failed = 0


def run(mode, tool):
    env = dict(os.environ, PYTHONPATH=ROOT)
    return subprocess.run([sys.executable, "-c", RUN, mode, tool], cwd=ROOT, env=env,
                          capture_output=True, text=True, timeout=60)


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def main():
    print("[-] host tools refuse Python < 3.11 with a clear message")
    for tool in TOOLS:
        r = run("old", tool)
        check(r.returncode == 1 and "needs Python 3.11 or newer" in r.stderr, "%s stops on 3.10" % tool,
              "rc=%d %s" % (r.returncode, (r.stderr or r.stdout).strip()[-200:]))
        r = run("current", tool)
        check(r.returncode == 0 and "needs Python" not in r.stderr, "%s runs on %d.%d" % (tool, *sys.version_info[:2]),
              "rc=%d %s" % (r.returncode, (r.stderr or r.stdout).strip()[-200:]))
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] all checks passed")


if __name__ == "__main__":
    main()
