#!/usr/bin/env python3
"""
POST /api/ethernet-save with "apply" runs network_manager.sh where the installers put it,
/data/harmony/network_manager.sh (deploy_hub.py and update.sh copy the scripts there; init.sh
looks there first). It used to run /data/harmony/bin/network_manager.sh, which no installer
creates, so the settings were saved but not applied. A shim records the system() calls instead
of running them; the daemon runs with --dry-run.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, BUILD_DIR, MOCK_LIB, build_mock_lib  # noqa: E402

PORT = 8979
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
SHIM = os.path.join(BUILD_DIR, "system_record_shim.so")


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    build_mock_lib()
    res = subprocess.run(["gcc", "-Wall", "-O2", "-fPIC", "-shared", os.path.join(TESTS_DIR, "system_record_shim.c"), "-o", SHIM],
                         capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        return 1
    fd, rec = tempfile.mkstemp(prefix="system_rec_")
    os.close(fd)
    try:
        env = {"LD_PRELOAD": MOCK_LIB + ":" + SHIM, "SYSTEM_RECORD_FILE": rec}
        with HubHarness(port=PORT, env=env) as hub:
            open(rec, "w").close()  # only what the request runs
            st, r = hub.http_post("/api/ethernet-save", {"eth_enabled": True, "eth_mode": "dhcp", "apply": "apply"})
            check(st == 200 and isinstance(r, dict) and r.get("ok") is True, "ethernet settings saved", "%s %s" % (st, r))
            cmds = [ln for ln in open(rec).read().splitlines() if "network_manager.sh" in ln]
            check(len(cmds) == 1 and cmds[0].startswith("/data/harmony/network_manager.sh apply"),
                  "apply runs /data/harmony/network_manager.sh", str(cmds))
            open(rec, "w").close()
            st, r = hub.http_post("/api/ethernet-save", {"eth_enabled": True, "eth_mode": "dhcp"})
            check(st == 200 and not [ln for ln in open(rec).read().splitlines() if "network_manager.sh" in ln],
                  "a save without apply runs nothing", open(rec).read())
    finally:
        os.unlink(rec)

    if failures:
        print("ethernet apply: %d FAILED" % len(failures))
        return 1
    print("all ethernet apply checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
