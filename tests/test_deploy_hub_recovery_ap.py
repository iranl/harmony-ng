#!/usr/bin/env python3
"""build/deploy_hub.py removes what the removed recovery access point left on a Hub.

RECOVERY_AP_CLEANUP runs with sh in a sandbox: /data, /tmp and /var/run are moved under a
temporary root, and kill, killall, ifconfig and wlanconfig are shell functions that record
their calls. Checks:
- recovery_ap.sh (all three places), recovery_ap.conf, the recovery log and the hub_portal and
  hub_dhcpd links are removed; hub_util and its other links (hub_sntp) stay;
- an access point still running (its pid file exists) is stopped: the portal loop is killed,
  hub_portal, hub_dhcpd and hostapd are ended and ath1 is destroyed;
- without that pid file nothing is killed and no interface is touched.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "build"))

import deploy_hub  # noqa: E402

OLD_FILES = (
    "data/harmony/recovery_ap.sh",
    "data/harmony/scripts/recovery_ap.sh",
    "data/harmony/bin/recovery_ap.sh",
    "data/harmony/recovery_ap.conf",
    "tmp/harmony-recovery.log",
)


class DeployHubRecoveryApCleanup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for d in ("data/harmony/bin", "data/harmony/scripts", "tmp", "var/run"):
            (self.root / d).mkdir(parents=True)
        for f in OLD_FILES:
            (self.root / f).write_text("old\n")
        bin_dir = self.root / "data" / "harmony" / "bin"
        (bin_dir / "hub_util").write_text("binary\n")
        for link in ("hub_portal", "hub_dhcpd", "hub_sntp"):
            (bin_dir / link).symlink_to(bin_dir / "hub_util")
        self.calls = self.root / "calls"

    def tearDown(self):
        self.tmp.cleanup()

    def run_cleanup(self) -> list[str]:
        # one pass: the sandbox root itself may be under /tmp
        cmd = re.sub(r"(^|[\s(])/(data|tmp|var/run)/", lambda m: f"{m.group(1)}{self.root}/{m.group(2)}/",
                     deploy_hub.RECOVERY_AP_CLEANUP)
        stubs = "".join(
            f'{name}() {{ echo "{name} $*" >> \'{self.calls}\'; }}\n'
            for name in ("kill", "killall", "ifconfig", "wlanconfig")
        )
        res = subprocess.run(["sh", "-c", stubs + cmd], capture_output=True, text=True, timeout=30)
        self.assertEqual(res.returncode, 0, res.stderr)
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def assert_files_removed(self):
        for f in OLD_FILES:
            self.assertFalse((self.root / f).exists(), f"{f} was left on the Hub")
        bin_dir = self.root / "data" / "harmony" / "bin"
        for link in ("hub_portal", "hub_dhcpd"):
            self.assertFalse(bin_dir.joinpath(link).is_symlink(), f"{link} link was left on the Hub")
        self.assertTrue((bin_dir / "hub_util").is_file(), "hub_util itself was removed")
        self.assertTrue((bin_dir / "hub_sntp").is_symlink(), "another hub_util link was removed")

    def test_running_access_point_stopped_and_removed(self):
        (self.root / "var" / "run" / "hub-recovery-ap.pid").write_text("4242\n")
        (self.root / "tmp" / "hostapd-recovery.conf").write_text("interface=ath1\n")
        calls = self.run_cleanup()
        self.assert_files_removed()
        self.assertFalse((self.root / "var" / "run" / "hub-recovery-ap.pid").exists())
        self.assertFalse((self.root / "tmp" / "hostapd-recovery.conf").exists())
        self.assertIn("kill -9 4242", calls)
        self.assertIn("killall -9 hub_portal hub_dhcpd hostapd", calls)
        self.assertIn("wlanconfig ath1 destroy", calls)

    def test_no_access_point_running(self):
        calls = self.run_cleanup()
        self.assert_files_removed()
        self.assertEqual(calls, [], "something was killed or an interface touched without a running access point")


if __name__ == "__main__":
    unittest.main(verbosity=2)
