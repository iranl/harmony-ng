#!/usr/bin/env python3
"""payload/scripts/network_manager.sh on the host (functions only, nothing touches the network).

The script's /data, /etc and /tmp paths are moved into a temporary directory, its final
command dispatch is cut off, and the functions are called from a small driver with the
network commands replaced by shell functions. Checks:
- ETH_ENABLED and ETH_FALLBACK_WIFI from ethernet.conf are read (they were fixed at 1);
- with Ethernet turned off, the boot does not start Ethernet (no USB host mode switch);
- a DNS list with two servers becomes two nameserver lines; only blanks or commas leave
  resolv.conf alone, and a "*" is not expanded to file names;
- wpa_supplicant is (re)started with /etc/wpa_supplicant.conf, the file every writer updates,
  also when the older wpa_supplicant.ath0.conf copy exists;
- a boot with no Ethernet link, no Wi-Fi link and no Wi-Fi network stored starts no access
  point: a recovery_ap.sh left by an older release is not run, and the scripts no longer
  name it;
- USB port mode (Wi-Fi setup over USB): with Ethernet on, a boot keeps the port a USB device
  and starts no Ethernet over USB only when usbhid connected to hub_usbwifi a second time (a
  computer set the device up), also when that happens during the wait; interrupts in /proc/udc
  alone only make the boot wait USB_PC_WAIT_IRQ seconds, then it switches to host mode (a stray
  interrupt from an adapter never costs Ethernet); with neither, it waits USB_PC_WAIT seconds at
  most; without /proc/udc it does not wait; USB_WIFI_SETUP=0 skips the wait and keeps host mode;
  the mode is written to /tmp/harmony_usb_port_mode (device when kept, also with Ethernet off);
  the onboard Ethernet of the Pro never looks at the USB port.
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
        text = text.replace("/proc/udc", f"{self.root}/proc_udc")
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
            "ensure_wifi() { return 0; }\nstart_monitor() { :; }\n"
            "killall() { :; }\nis_onboard_eth() { return 1; }\n"
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

    def test_dns_blank_keeps_resolv_conf(self):
        resolv = self.root / "etc" / "resolv.conf"
        resolv.write_text("nameserver 192.168.1.1\n")
        self.conf.write_text('ETH_MODE="static"\nETH_DNS=" , "\n')
        self.run_sh("load_config; write_resolv_conf")
        self.assertEqual(resolv.read_text(), "nameserver 192.168.1.1\n", "only blanks and commas: no empty nameserver line")

    def test_dns_wildcard_not_expanded(self):
        self.conf.write_text('ETH_MODE="static"\nETH_DNS="*"\n')
        self.run_sh("load_config; write_resolv_conf")
        self.assertEqual((self.root / "etc" / "resolv.conf").read_text(), "nameserver *\n", "a * stays a *, not the file names")

    def test_wpa_conf_is_the_main_file(self):
        (self.root / "etc" / "wpa_supplicant.conf").write_text("network={\n}\n")
        (self.root / "etc" / "wpa_supplicant.ath0.conf").write_text("network={\n}\n")
        out = self.run_sh('echo "W=$WPA_CONF"')
        self.assertIn(f"W={self.root}/etc/wpa_supplicant.conf\n", out)
        (self.root / "etc" / "wpa_supplicant.conf").unlink()
        out = self.run_sh('echo "W=$WPA_CONF"')
        self.assertIn(f"W={self.root}/etc/wpa_supplicant.ath0.conf\n", out)


    def test_boot_without_link_starts_no_access_point(self):
        marker = self.root / "ap_started"
        # what an older release left on the Hub
        for d in ("data/harmony", "data/harmony/scripts", "data/harmony/bin"):
            old = self.root / d / "recovery_ap.sh"
            old.parent.mkdir(parents=True, exist_ok=True)
            old.write_text(f"#!/bin/sh\necho \"$*\" >> '{marker}'\n")
            old.chmod(0o755)
        stubs = (
            "start_eth() { return 1; }\nensure_wifi() { return 1; }\nstart_monitor() { echo MONITOR; }\n"
            "killall() { :; }\nis_onboard_eth() { return 1; }\nfind_eth_interface() { return 1; }\n"
        )
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        out = self.run_sh(stubs + "do_boot")
        self.assertIn("MONITOR", out, "the link monitor (Wi-Fi retries, Ethernet) still starts")
        self.assertFalse(marker.exists(), "an access point script was run")
        self.assertFalse((self.root / "run" / "hub-recovery-ap.pid").exists())
        log = self.root / "tmp" / "harmony-network.log"
        self.assertNotIn("recovery", log.read_text().lower() if log.exists() else "")
        for name in ("network_manager.sh", "network_monitor.sh", "init.sh"):
            text = (SCRIPTS / name).read_text()
            for word in ("recovery_ap", "hub_portal", "hub_dhcpd", "hub-recovery-ap"):
                self.assertNotIn(word, text, f"{name} still refers to {word}")

    # ---- USB port mode: a computer at boot keeps the port a USB device ----

    PORT_STUBS = (
        "start_eth() { echo START_ETH; return 1; }\nensure_wifi() { return 0; }\nstart_monitor() { :; }\n"
        "killall() { :; }\nfind_eth_interface() { return 1; }\nsleep() { echo SLEPT; }\n"
    )

    def udc(self, count: int) -> None:
        (self.root / "proc_udc").write_text(
            "Atheros USB Device Controller\nTotal interrupt count = %d\nEndpoints: 3\n" % count)

    def test_udc_interrupt_count_read(self):
        self.udc(1234)
        self.assertIn("I=1234\n", self.run_sh('echo "I=$(udc_interrupts)"'))

    def mode(self) -> str:
        p = self.root / "tmp" / "harmony_usb_port_mode"
        return p.read_text().strip() if p.exists() else ""

    def test_interrupts_alone_wait_longer_then_host_mode(self):
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        self.udc(57)
        (self.root / "tmp" / "harmony_usbwifi.state").write_text("connections=1\nrequests=0\nlast_attempt=none\n")
        out = self.run_sh(self.PORT_STUBS + "is_onboard_eth() { return 1; }\ndo_boot")
        self.assertIn("START_ETH", out, "an interrupt count alone kept the port a device (Ethernet adapter lost)")
        self.assertEqual(out.count("SLEPT"), 20, "waits USB_PC_WAIT_IRQ seconds for the second usbhid connection")
        log = (self.root / "tmp" / "harmony-network.log").read_text()
        self.assertIn("interrupts seen", log)
        self.assertNotIn("A computer is using the USB port", log)

    def test_computer_during_the_wait_keeps_device_mode(self):
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        self.udc(57)
        state = self.root / "tmp" / "harmony_usbwifi.state"
        state.write_text("connections=1\n")
        stubs = self.PORT_STUBS.replace(
            "sleep() { echo SLEPT; }",
            f"sleep() {{ echo SLEPT; _n=$((${{_n:-0}} + 1)); [ \"$_n\" -eq 9 ] && echo connections=2 > '{state}'; :; }}")
        out = self.run_sh(stubs + "is_onboard_eth() { return 1; }\ndo_boot")
        self.assertNotIn("START_ETH", out, "the second usbhid connection 9 s in was not waited for")
        self.assertEqual(out.count("SLEPT"), 9)
        self.assertIn("A computer is using the USB port", (self.root / "tmp" / "harmony-network.log").read_text())
        self.assertEqual(self.mode(), "device")

    def test_second_usbhid_connection_keeps_device_mode(self):
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        self.udc(0)
        (self.root / "tmp" / "harmony_usbwifi.state").write_text("connections=2\nrequests=0\nlast_attempt=none\n")
        out = self.run_sh(self.PORT_STUBS + "is_onboard_eth() { return 1; }\ndo_boot")
        self.assertNotIn("START_ETH", out)
        self.assertNotIn("SLEPT", out)
        self.assertEqual(self.mode(), "device")

    def test_request_on_first_connection_keeps_device_mode(self):
        # a host that set the device up only once, or a tool already talking to the Hub
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        self.udc(57)
        state = self.root / "tmp" / "harmony_usbwifi.state"
        state.write_text("connections=1\nrequests=0\nlast_attempt=none\n")
        stubs = self.PORT_STUBS.replace(
            "sleep() { echo SLEPT; }",
            f"sleep() {{ echo SLEPT; _n=$((${{_n:-0}} + 1)); [ \"$_n\" -eq 3 ] && "
            f"printf 'connections=1\\nrequests=1\\nlast_attempt=none\\n' > '{state}'; :; }}")
        out = self.run_sh(stubs + "is_onboard_eth() { return 1; }\ndo_boot")
        self.assertNotIn("START_ETH", out, "a request from the computer did not keep the port a device")
        self.assertEqual(out.count("SLEPT"), 3)
        self.assertEqual(self.mode(), "device")

    def test_setting_off_skips_the_wait(self):
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\nUSB_WIFI_SETUP=0\n')
        self.udc(57)
        (self.root / "tmp" / "harmony_usbwifi.state").write_text("connections=2\n")
        out = self.run_sh(self.PORT_STUBS + "is_onboard_eth() { return 1; }\ndo_boot")
        self.assertIn("START_ETH", out, "USB_WIFI_SETUP=0: Ethernet over USB starts as before")
        self.assertNotIn("SLEPT", out)

    def test_ethernet_off_marks_device_mode(self):
        self.conf.write_text('ETH_ENABLED=0\nETH_MODE="dhcp"\n')
        self.run_sh(self.PORT_STUBS + "is_onboard_eth() { return 1; }\ndo_boot")
        self.assertEqual(self.mode(), "device")

    def test_no_computer_switches_to_host_mode(self):
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        self.udc(0)
        (self.root / "tmp" / "harmony_usbwifi.state").write_text("connections=1\nrequests=0\nlast_attempt=none\n")
        out = self.run_sh(self.PORT_STUBS + "is_onboard_eth() { return 1; }\ndo_boot")
        self.assertIn("START_ETH", out, "a charger or an adapter on the port: Ethernet over USB as before")
        self.assertEqual(out.count("SLEPT"), 6, "waits USB_PC_WAIT seconds for a computer, no more")

    def test_no_udc_no_wait(self):
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        out = self.run_sh(self.PORT_STUBS + "is_onboard_eth() { return 1; }\ndo_boot")
        self.assertIn("START_ETH", out)
        self.assertNotIn("SLEPT", out, "no device controller: no wait")

    def test_onboard_ethernet_ignores_usb_port(self):
        self.conf.write_text('ETH_ENABLED=1\nETH_MODE="dhcp"\n')
        self.udc(57)
        out = self.run_sh(self.PORT_STUBS + "is_onboard_eth() { return 0; }\ndo_boot")
        self.assertIn("START_ETH", out)


if __name__ == "__main__":
    unittest.main()
