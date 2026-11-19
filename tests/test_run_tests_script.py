#!/usr/bin/env python3
"""
tests/run_tests.sh must fail when the host test daemon does not compile.

It used to print "Host test compile skipped", then, without a binary, a "Dry-run logic
verified" success line, and exit 0 without running a single API test.

The script runs in a sandbox (a copy of tests/run_tests.sh and tests/hub_harness.py, the real
payload/ linked in) with a gcc wrapper first in PATH that fails for the daemon build and
passes everything else through. No daemon is started, no port is used.
"""

import os
import shutil
import subprocess
import sys
import tempfile

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TESTS_DIR)

# Fails the daemon build; the RF loopback test becomes a program that does nothing (it would
# bind hub_rf's port); everything else goes to the real gcc.
FAKE_GCC = r'''#!/bin/sh
out=""; prev=""; stub=0
for a in "$@"; do
    case "$a" in
        *hub_daemon.c) echo "fake gcc: refusing the daemon build" >&2; exit 1 ;;
        *test_rf_loopback.c) stub=1 ;;
    esac
    [ "$prev" = "-o" ] && out="$a"
    prev="$a"
done
if [ "$stub" = 1 ]; then printf '#!/bin/sh\nexit 0\n' > "$out"; chmod +x "$out"; exit 0; fi
exec "$REAL_GCC" "$@"
'''


def main():
    real_gcc = shutil.which("gcc")
    if not real_gcc:
        print("[-] gcc not found")
        sys.exit(1)
    box = tempfile.mkdtemp(prefix="run_tests_box_")
    try:
        os.makedirs(os.path.join(box, "tests", "mock_hardware"))
        for name in ("run_tests.sh", "hub_harness.py", "test_rf_loopback.c"):
            shutil.copy(os.path.join(TESTS_DIR, name), os.path.join(box, "tests", name))
        shutil.copy(os.path.join(TESTS_DIR, "mock_hardware", "mock_hardware.c"),
                    os.path.join(box, "tests", "mock_hardware", "mock_hardware.c"))
        os.symlink(os.path.join(REPO_ROOT, "payload"), os.path.join(box, "payload"))
        bindir = os.path.join(box, "fakebin")
        os.makedirs(bindir)
        with open(os.path.join(bindir, "gcc"), "w") as f:
            f.write(FAKE_GCC)
        os.chmod(os.path.join(bindir, "gcc"), 0o755)
        env = dict(os.environ)
        env["PATH"] = bindir + os.pathsep + env.get("PATH", "")
        env["REAL_GCC"] = real_gcc
        env["TEST_PORT"] = "9264"
        res = subprocess.run(["bash", os.path.join(box, "tests", "run_tests.sh")], env=env,
                             capture_output=True, text=True, timeout=300)
        out = res.stdout + res.stderr
        failed = 0
        if res.returncode == 0:
            failed += 1
            print("  FAIL run_tests.sh exits 0 when the daemon does not compile")
        else:
            print("  PASS run_tests.sh exits %d when the daemon does not compile" % res.returncode)
        if "verified" in out or "ALL TESTS PASSED" in out:
            failed += 1
            print("  FAIL run_tests.sh reports success without testing")
        else:
            print("  PASS no success message")
        if failed:
            print(out[-2000:])
            sys.exit(1)
        print("[+] run_tests.sh stops on a failed build")
    finally:
        shutil.rmtree(box, ignore_errors=True)


if __name__ == "__main__":
    main()
