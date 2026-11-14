#!/usr/bin/env python3
"""Recovery AP fallback and button monitor, run on the host.

payload/scripts/network_manager.sh (with network_monitor.sh), recovery_ap.sh and the
recovery AP block of init.sh run with sh in a sandbox: every absolute device path is moved
under a temporary root and the network commands are stubs first in PATH. sleep is a stub
that advances a simulated clock (also written to <root>/proc/uptime) and applies the
scenario's events, so minutes of monitor cycles run in a few seconds.
"""

from __future__ import annotations

import os
import pathlib
import re
import signal
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPTS = REPO_DIR / "payload" / "scripts"

DEVICE_DIRS = "data|tmp|etc|var|sys|proc|cache|usr|sbin"

STUBS = {
    # wpa_state comes from <root>/ctl/wpa_state
    "wpa_cli": 'case "$*" in *status*) echo "wpa_state=$(cat "$R/ctl/wpa_state")";; esac; exit 0',
    # interface queries: ath0 never has an address; an Ethernet interface has one while it has carrier
    "ifconfig": """if [ "$#" -eq 1 ]; then
  echo "$1      Link encap:Ethernet  HWaddr 00:11:22:33:44:55"
  if [ "$1" != ath0 ] && [ "$(cat "$R/sys/class/net/$1/carrier" 2>/dev/null)" = 1 ]; then
    echo "          inet addr:192.0.2.60  Bcast:192.0.2.255  Mask:255.255.255.0"
  fi
fi; exit 0""",
    # no routes: "route del" fails (migrate_to_wifi deletes default routes until it does)
    "route": 'case "${1:-}" in -n) echo "Kernel IP routing table";; del) exit 1;; esac; exit 0',
    "ps": 'echo "  PID USER       VSZ STAT COMMAND"; echo "  321 root      1234 S    wpa_supplicant -s -iath0 -c conf"',
    "pidof": "exit 1",
    "ping": "exit 1",
    "udhcpc": "exit 0",
    "killall": "exit 0",
    "wlanconfig": "exit 0",
    "iwpriv": "exit 0",
    "iwconfig": "exit 0",
    "sleep": """c=$(cat "$R/clock"); c=$((c + ${1%%.*})); echo "$c" > "$R/clock"
echo "$c.00 0.00" > "$R/proc/uptime"
. "$R/ctl/events.sh"
/bin/sleep 0.01
if [ "$c" -ge "$(cat "$R/ctl/limit")" ]; then kill -9 "$(cat "$R/main.pid")"; fi
exit 0""",
}

# recovery_ap.sh as network_manager.sh calls it: records each call with the simulated time
AP_STUB = """case "$1" in
  start) echo "start $(cat "$R/clock")" >> "$R/ap_calls"; echo 999 > "$R/var/run/hub-recovery-ap.pid" ;;
  stop) echo "stop $(cat "$R/clock")" >> "$R/ap_calls"; rm -f "$R/var/run/hub-recovery-ap.pid" ;;
  *) echo "$1" >> "$R/ap_calls" ;;
esac"""

WPA_CONF = """ctrl_interface=/var/run/wpa_supplicant
update_config=1

network={
    ssid="ExampleNet"
    psk="not-the-routers-pass-phrase"
}
"""


class Sandbox:
    def __init__(self, tmp: pathlib.Path):
        self.tmp = tmp
        self.root = tmp / "root"
        self.stub = tmp / "stub"
        for d in ("stub", "root/ctl", "root/data/harmony", "root/tmp", "root/var/run", "root/etc",
                  "root/proc", "root/sbin", "root/usr/sbin", "root/sys/class/input/input0",
                  "root/sys/class/net/lo", "root/sys/class/net/wifi0", "root/sys/class/net/ath0",
                  "root/sys/class/net/eth0", "root/sys/class/net/eth1"):
            (tmp / d).mkdir(parents=True, exist_ok=True)
        (self.root / "sys/class/net/eth1/carrier").write_text("0\n")  # standard Hub: no PHY on eth1
        (self.root / "sys/class/net/wifi0/address").write_text("00:11:22:33:44:55\n")
        (self.root / "etc/wpa_supplicant.conf").write_text(WPA_CONF)
        (self.root / "clock").write_text("0\n")
        (self.root / "proc/uptime").write_text("0.00 0.00\n")
        (self.root / "ctl/wpa_state").write_text("SCANNING\n")
        (self.root / "ctl/events.sh").write_text(":\n")
        for name, body in STUBS.items():
            self.script(self.stub / name, body)
        # absolute calls in the scripts
        self.script(self.root / "sbin/wlanconfig", "exit 0")
        self.script(self.root / "usr/sbin/wpa_supplicant", "exit 0")

    def script(self, path: pathlib.Path, body: str) -> None:
        path.write_text(f'#!/bin/sh\nR="{self.root}"\n{body}\n')
        path.chmod(0o755)

    def sandboxed(self, name: str, dest: pathlib.Path) -> pathlib.Path:
        text = (SCRIPTS / name).read_text()
        text = re.sub(r"(?<![\w./$-])/(" + DEVICE_DIRS + r")/", lambda m: f"{self.root}/{m.group(1)}/", text)
        text = re.sub(r"(?m)^PATH=.*$", f"PATH={self.stub}:/usr/bin:/bin", text)
        dest.write_text(text)
        dest.chmod(0o755)
        return dest

    def events(self, body: str) -> None:
        (self.root / "ctl/events.sh").write_text(body + "\n")

    def run(self, script: pathlib.Path, arg: str, limit: int) -> None:
        (self.root / "ctl/limit").write_text(f"{limit}\n")
        env = dict(os.environ, PATH=f"{self.stub}:/usr/bin:/bin")
        cmd = f'echo $$ > "{self.root}/main.pid"; exec sh "{script}" {arg}'
        proc = subprocess.Popen(["sh", "-c", cmd], env=env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            proc.wait(timeout=120)
        finally:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def clock(self) -> int:
        return int((self.root / "clock").read_text())


class NetworkFallback(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.sb = Sandbox(pathlib.Path(self.tmpdir.name))
        harmony = self.sb.root / "data/harmony"
        self.sb.sandboxed("network_monitor.sh", harmony / "network_monitor.sh")
        self.nm = self.sb.sandboxed("network_manager.sh", harmony / "network_manager.sh")
        self.sb.script(harmony / "recovery_ap.sh", AP_STUB)

    def tearDown(self):
        self.tmpdir.cleanup()

    def calls(self) -> list[tuple[str, int]]:
        f = self.sb.root / "ap_calls"
        if not f.exists():
            return []
        return [(w[0], int(w[1])) for w in (line.split() for line in f.read_text().splitlines())]

    def monitor(self, limit: int) -> None:
        self.sb.run(self.nm, "monitor", limit)
        self.assertGreaterEqual(self.sb.clock(), limit, "the monitor ran to the end of the scenario")

    def add_usb_adapter(self, carrier: str = "0") -> None:
        eth2 = self.sb.root / "sys/class/net/eth2"
        eth2.mkdir()
        (eth2 / "carrier").write_text(carrier + "\n")

    def test_saved_network_that_never_associates_starts_the_ap(self):
        self.monitor(300)
        calls = self.calls()
        self.assertEqual([c[0] for c in calls], ["start"], f"recovery AP calls: {calls}")
        self.assertTrue(180 <= calls[0][1] <= 186, f"started at {calls[0][1]} s, expected just after 180 s")

    def test_associated_wifi_never_starts_the_ap(self):
        (self.sb.root / "ctl/wpa_state").write_text("COMPLETED\n")
        self.monitor(400)
        self.assertEqual(self.calls(), [])

    def test_wifi_that_worked_and_dropped_is_retried_not_replaced(self):
        (self.sb.root / "ctl/wpa_state").write_text("COMPLETED\n")
        self.sb.events('[ "$c" -ge 60 ] && echo SCANNING > "$R/ctl/wpa_state"')
        self.monitor(400)
        self.assertEqual(self.calls(), [])

    def test_ethernet_link_stops_the_ap(self):
        self.add_usb_adapter("0")
        self.sb.events('[ "$c" -ge 240 ] && echo 1 > "$R/sys/class/net/eth2/carrier"')
        self.monitor(300)
        calls = self.calls()
        self.assertEqual([c[0] for c in calls], ["start", "stop"], f"recovery AP calls: {calls}")
        self.assertTrue(180 <= calls[0][1] <= 186)
        self.assertTrue(240 <= calls[1][1] <= 246)
        self.assertFalse((self.sb.root / "var/run/hub-recovery-ap.pid").exists())

    def test_ethernet_carrier_at_boot_never_starts_the_ap(self):
        self.add_usb_adapter("1")
        self.monitor(300)
        self.assertEqual(self.calls(), [])

    def test_static_fallback_does_not_stop_the_ap_without_a_link(self):
        (self.sb.root / "data/harmony/ethernet.conf").write_text(
            'ETH_MODE="static"\nETH_IP="192.0.2.50"\nETH_NETMASK="255.255.255.0"\nETH_GATEWAY="192.0.2.1"\n')
        self.add_usb_adapter("0")
        self.monitor(300)
        calls = self.calls()
        self.assertEqual([c[0] for c in calls], ["start"], f"recovery AP calls: {calls}")
        self.assertTrue((self.sb.root / "var/run/hub-recovery-ap.pid").exists(), "the AP is still running")


class ButtonMonitor(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.sb = Sandbox(pathlib.Path(self.tmpdir.name))
        self.ap = self.sb.sandboxed("recovery_ap.sh", self.sb.root / "data/harmony/recovery_ap.sh")
        self.tde = self.sb.root / "sys/class/input/input0/tde"

    def tearDown(self):
        self.tmpdir.cleanup()

    def started(self) -> bool:
        log = self.sb.root / "tmp/harmony-recovery.log"
        return log.exists() and "starting recovery ap" in log.read_text()

    def test_value_one_from_the_start_is_ignored(self):
        self.tde.write_text("1\n")
        self.sb.run(self.ap, "monitor", 60)
        self.assertFalse(self.started(), "a node reading 1 from boot must not start the AP")

    def test_change_to_one_for_five_seconds_starts_the_ap(self):
        self.tde.write_text("0\n")
        self.sb.events('[ "$c" -ge 10 ] && echo 1 > "$R/sys/class/input/input0/tde"')
        self.sb.run(self.ap, "monitor", 30)
        self.assertTrue(self.started())

    def test_short_press_does_not_start_the_ap(self):
        self.tde.write_text("0\n")
        self.sb.events('if [ "$c" -ge 10 ] && [ "$c" -lt 13 ]; then echo 1; else echo 0; fi > "$R/sys/class/input/input0/tde"')
        self.sb.run(self.ap, "monitor", 30)
        self.assertFalse(self.started())

    def test_init_starts_the_monitor(self):
        text = (SCRIPTS / "init.sh").read_text()
        m = re.search(r"(?ms)^# 5\.3 Recovery AP button monitor.*?(?=^# 6\. )", text)
        self.assertIsNotNone(m, "init.sh has a recovery AP monitor block before section 6")
        block = re.sub(r"(?<![\w./$-])/(" + DEVICE_DIRS + r")/", lambda x: f"{self.sb.root}/{x.group(1)}/", m.group(0))
        self.sb.script(self.sb.root / "data/harmony/recovery_ap.sh", 'echo "$1" >> "$R/ap_calls"')
        script = self.sb.tmp / "init_block.sh"
        script.write_text(f'LOG="{self.sb.root}/tmp/harmony-init.log"\n{block}\nwait\n')
        subprocess.run(["sh", str(script)], timeout=30, check=True)
        self.assertEqual((self.sb.root / "ap_calls").read_text().split(), ["monitor"])
        pids = (self.sb.root / "tmp/harmony_supervisors.pid").read_text().split()
        self.assertEqual(len(pids), 1, "the monitor's pid is recorded, so init.sh stop ends it")


if __name__ == "__main__":
    unittest.main(verbosity=2)
