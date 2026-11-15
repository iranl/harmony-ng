#!/usr/bin/env python3
"""payload/scripts/network_manager.sh on the host (functions only, nothing touches the network).

The script's /data, /etc and /tmp paths are moved into a temporary directory, its final
command dispatch is cut off, and the functions are called from a small driver with the
network commands replaced by shell functions. Checks:
- ETH_ENABLED and ETH_FALLBACK_WIFI from ethernet.conf are read (they were fixed at 1);
- with Ethernet turned off, the boot does not start Ethernet (no USB host mode switch);
- a DNS list with two servers becomes two nameserver lines;
- wpa_supplicant is (re)started with /etc/wpa_supplicant.conf, the file every writer updates,
  also when the older wpa_supplicant.ath0.conf copy exists.
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPTS = REPO_DIR / "payload" / "scripts"


class NetworkManagerScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for d in ("tmp", "etc", "data/harmony", "run"):
            (self.root / d).mkdir(parents=True)
        text = (SCRIPTS / "network_manager.sh").read_text()
        text = text.replace("/tmp/", f"{self.root}/tmp/")
        text = text.replace("/data/harmony", f"{self.root}/data/harmony")
        text = text.replace("/etc/", f"{self.root}/etc/")
        text = text.replace("/var/run/", f"{self.root}/run/")
        # functions only: drop the command dispatch at the end
        text = text[: text.rindex('case "$1" in')]
        self.lib = self.root / "nm_lib.sh"
        self.lib.write_text(text)
        shutil.copy(SCRIPTS / "network_monitor.sh", self.root / "network_monitor.sh")
        self.conf = self.root / "data" / "harmony" / "ethernet.conf"

    def tearDown(self):
        self.tmp.cleanup()

    def run_sh(self, body: str) -> str:
        script = f". '{self.lib}'\n{body}\n"
        res = subprocess.run(["sh", "-c", script], capture_output=True, text=True, timeout=30, cwd=self.root)
        self.assertEqual(res.returncode, 0, res.stderr)
        return res.stdout

    def test_eth_flags_read_from_conf(self):
        self.conf.write_text('ETH_ENABLED=0\nETH_FALLBACK_WIFI=0\nETH_MODE="dhcp"\n')
        out = self.run_sh('load_config; echo "E=$ETH_ENABLED F=$ETH_FALLBACK_WIFI"')
        self.assertIn("E=0 F=0", out)
        self.conf.write_text('ETH_MODE="dhcp"\n')
        out = self.run_sh('load_config; echo "E=$ETH_ENABLED F=$ETH_FALLBACK_WIFI"')
        self.assertIn("E=1 F=1", out, "missing keys keep the default 1")

    def test_boot_skips_ethernet_when_disabled(self):
        stubs = (
            "start_eth() { echo START_ETH; return 1; }\n"
            "ensure_wifi() { return 0; }\nstart_monitor() { :; }\nstop_recovery_ap() { :; }\n"
            "start_recovery_ap() { :; }\nkillall() { :; }\nis_onboard_eth() { return 1; }\n"
        )
        self.conf.write_text('ETH_ENABLED=0\nETH_MODE="dhcp"\n')
        self.assertNotIn("START_ETH", self.run_sh(stubs + "do_boot"))
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        self.assertIn("START_ETH", self.run_sh(stubs + "do_boot"))

    def test_dns_list_one_line_per_server(self):
        self.conf.write_text('ETH_MODE="static"\nETH_DNS="192.168.1.1, 9.9.9.9"\n')
        self.run_sh("load_config; write_resolv_conf")
        resolv = (self.root / "etc" / "resolv.conf").read_text()
        self.assertEqual(resolv, "nameserver 192.168.1.1\nnameserver 9.9.9.9\n")

    def test_wpa_conf_is_the_main_file(self):
        (self.root / "etc" / "wpa_supplicant.conf").write_text("network={\n}\n")
        (self.root / "etc" / "wpa_supplicant.ath0.conf").write_text("network={\n}\n")
        out = self.run_sh('echo "W=$WPA_CONF"')
        self.assertIn(f"W={self.root}/etc/wpa_supplicant.conf\n", out)
        (self.root / "etc" / "wpa_supplicant.conf").unlink()
        out = self.run_sh('echo "W=$WPA_CONF"')
        self.assertIn(f"W={self.root}/etc/wpa_supplicant.ath0.conf\n", out)


if __name__ == "__main__":
    unittest.main()
