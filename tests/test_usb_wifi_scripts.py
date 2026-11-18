#!/usr/bin/env python3
"""Wi-Fi setup over USB: the shell parts, run on the host in sandboxes.

Every absolute device path of the script under test is moved into a temporary root, and the
commands with side effects are stubs first in PATH (they record their calls). Nothing on the
host is touched; the only real processes are the test's own.

- start_usb_eth.sh: "host" is written to /tmp/harmony_usb_port_mode before usbhid is killed;
  the script waits while a killed usbhid is still there, and tries the driver unload again
  while ath_udc stays in /proc/modules.
- init.sh 5.1 (the hub_usbwifi supervisor): starts the applet; USB_WIFI_SETUP=0 in
  ethernet.conf starts nothing; on a later start it starts usbhid again only while the port is
  a USB device (never once the mode file says host, also with ath_udc still loaded), and only
  after the killed one is gone and gadgetfs is unmounted.
- init.sh stop: the applet started as "hub_util usbwifi" (no hub_usbwifi link) is killed;
  another hub_util process is left alone.
- rcS.local emergency mode: starts "hub_util usbwifi" and usbhid, and neither with
  USB_WIFI_SETUP=0; its udhcpc writes the pid file the applet signals for a new lease.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPTS = REPO_DIR / "payload" / "scripts"
DEVICE_DIRS = "data|tmp|etc|root|home|usr|sbin|bin|cache|proc|sys|opt|lib|var"


class Sandbox:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.root = base / "root"
        self.stub = base / "stub"
        self.calls = base / "calls.log"
        for d in ("data/harmony/bin", "tmp", "usr/bin", "proc", "stub"):
            (self.root / d).mkdir(parents=True, exist_ok=True)
        self.stub.mkdir()
        self.env = dict(os.environ, PATH=f"{self.stub}:/usr/bin:/bin")

    def script(self, name: str, body: str) -> pathlib.Path:
        p = self.stub / name
        p.write_text("#!/bin/sh\n" + body)
        p.chmod(0o755)
        return p

    def recorder(self, name: str, extra: str = "") -> None:
        self.script(name, f'echo "{name} $*" >> \'{self.calls}\'\n{extra}exit 0\n')

    def device_file(self, rel: str, body: str) -> pathlib.Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("#!/bin/sh\n" + body)
        p.chmod(0o755)
        return p

    def rewrite(self, text: str) -> str:
        return re.sub(r"(?<![\w./$-])/(" + DEVICE_DIRS + r")/", lambda m: f"{self.root}/{m.group(1)}/", text)

    def run(self, text: str, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(["sh", "-c", text], env=self.env, capture_output=True, text=True, timeout=timeout)

    def call_lines(self) -> list[str]:
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def cleanup(self):
        self.tmp.cleanup()


class StartUsbEth(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()
        sb = self.sb
        mode = sb.root / "tmp" / "harmony_usb_port_mode"
        modules = sb.root / "proc" / "modules"
        modules.write_text("ath_udc 12345 0 - Live 0xc0000000\ngadgetfs 1 0 - Live 0xc0001000\n")
        # killall records whether the mode file said host when usbhid was killed
        sb.recorder("killall", f"echo \"mode-at-kill $(cat '{mode}' 2>/dev/null)\" >> '{sb.calls}'\n")
        # usbhid is still there for the first two checks after the kill
        count = sb.root / "pidof_count"
        sb.script("pidof", f"n=$(cat '{count}' 2>/dev/null || echo 0); n=$((n + 1)); echo $n > '{count}'\n"
                           f"echo \"pidof $*\" >> '{sb.calls}'\n[ $n -le 2 ] && echo 4242 && exit 0\nexit 1\n")
        # ath_udc stays loaded after the first unload attempt
        rm_count = sb.root / "rmmod_count"
        sb.script("rmmod", f"echo \"rmmod $*\" >> '{sb.calls}'\n"
                           f"if [ \"$1\" = ath_udc ]; then n=$(cat '{rm_count}' 2>/dev/null || echo 0); n=$((n + 1));"
                           f" echo $n > '{rm_count}'; [ $n -ge 2 ] && : > '{modules}'; fi\nexit 0\n")
        for name in ("umount", "insmod", "sleep"):
            sb.recorder(name)
        self.mode = mode

    def tearDown(self):
        self.sb.cleanup()

    def test_host_mode_written_first_and_driver_unloaded(self):
        text = self.sb.rewrite((SCRIPTS / "start_usb_eth.sh").read_text())
        res = self.sb.run(text)
        calls = self.sb.call_lines()
        self.assertEqual(self.mode.read_text().strip(), "host")
        self.assertIn("mode-at-kill host", calls, "usbhid was killed before the mode file said host")
        first_umount = next(i for i, c in enumerate(calls) if c.startswith("umount"))
        pidofs = [i for i, c in enumerate(calls) if c == "pidof usbhid"]
        self.assertEqual(len(pidofs), 3, "waits while the killed usbhid is still there")
        self.assertLess(pidofs[-1], first_umount, "gadgetfs unmounted while usbhid was still there")
        self.assertEqual(sum(1 for c in calls if c == "rmmod ath_udc"), 2, "the unload was not tried again")
        self.assertNotIn("could not be unloaded", res.stdout)


class InitSupervisor(unittest.TestCase):
    """init.sh 5.1, run in the foreground for one round: the fake applet stops the loop"""

    def setUp(self):
        self.sb = Sandbox()
        sb = self.sb
        stop = sb.root / "tmp" / "harmony_stop"
        sb.device_file("data/harmony/bin/hub_util", f'echo "hub_util $*" >> \'{sb.calls}\'\n')
        sb.device_file("data/harmony/bin/hub_usbwifi", f'echo "hub_usbwifi $*" >> \'{sb.calls}\'\n: > \'{stop}\'\n')
        sb.device_file("usr/bin/usbhid", f'echo "usbhid $*" >> \'{sb.calls}\'\n')
        (sb.root / "proc" / "modules").write_text("ath_udc 12345 0 - Live 0xc0000000\n")
        for name in ("killall", "umount", "sleep"):
            sb.recorder(name)

    def tearDown(self):
        self.sb.cleanup()

    def run_block(self) -> None:
        text = (SCRIPTS / "init.sh").read_text()
        start = text.index("# 5.1 Wi-Fi setup over USB")
        end = text.index("# 5.2 Network Manager")
        block = text[start:end].replace(') &\necho "$!" >> /tmp/harmony_supervisors.pid', ')\n:', 1)
        self.assertNotEqual(block, text[start:end], "supervisor block layout changed")
        script = (f'LOG="{self.sb.root}/tmp/harmony-init.log"\nlink_if_changed() {{ :; }}\n'
                  + self.sb.rewrite(block))
        res = self.sb.run(script)
        self.assertEqual(res.returncode, 0, res.stderr)
        time.sleep(0.2)  # usbhid is started in the background

    def test_first_start_runs_the_applet_only(self):
        self.run_block()
        calls = self.sb.call_lines()
        self.assertIn("hub_usbwifi ", calls)
        self.assertFalse(any(c.startswith("usbhid") for c in calls), "the stock boot script starts usbhid the first time")

    def test_setting_off_starts_nothing(self):
        (self.sb.root / "data" / "harmony" / "ethernet.conf").write_text('ETH_ENABLED=1\nUSB_WIFI_SETUP="0"\n')
        self.run_block()
        self.assertEqual(self.sb.call_lines(), [])
        self.assertIn("turned off", (self.sb.root / "tmp" / "harmony-init.log").read_text())

    def test_restart_in_device_mode_starts_usbhid_again(self):
        sb = self.sb
        (sb.root / "tmp" / "harmony_usbwifi.started").write_text("")
        (sb.root / "tmp" / "harmony_usb_port_mode").write_text("device\n")
        # the killed usbhid is still listed twice before it is gone
        count = sb.root / "pidof_count"
        sb.script("pidof", f"n=$(cat '{count}' 2>/dev/null || echo 0); n=$((n + 1)); echo $n > '{count}'\n"
                           f"echo \"pidof $*\" >> '{sb.calls}'\n[ $n -le 2 ] && echo 4242 && exit 0\nexit 1\n")
        self.run_block()
        calls = sb.call_lines()
        self.assertIn("killall -9 usbhid", calls)
        self.assertIn("usbhid -s", calls)
        pidofs = [i for i, c in enumerate(calls) if c == "pidof usbhid"]
        self.assertEqual(len(pidofs), 3, "waits while the killed usbhid is still there")
        umount = calls.index("umount /dev/gadget")
        self.assertLess(pidofs[-1], umount, "gadgetfs unmounted while usbhid was still there")
        self.assertLess(umount, calls.index("usbhid -s"))

    def test_restart_in_host_mode_leaves_usbhid_alone(self):
        (self.sb.root / "tmp" / "harmony_usbwifi.started").write_text("")
        (self.sb.root / "tmp" / "harmony_usb_port_mode").write_text("host\n")
        self.run_block()  # ath_udc still listed: the unload failed
        calls = self.sb.call_lines()
        self.assertIn("hub_usbwifi ", calls)
        self.assertFalse(any(c.startswith("usbhid") for c in calls), "usbhid started on a port in host mode")
        self.assertNotIn("killall -9 usbhid", calls)


class InitStopFallback(unittest.TestCase):
    def test_hub_util_usbwifi_killed_others_kept(self):
        sb = Sandbox()
        procs = [subprocess.Popen(["sh", "-c", "sleep 30", "usbwifi"]),
                 subprocess.Popen(["sh", "-c", "sleep 30", "sntp"])]
        try:
            sb.script("pidof", f'echo "{procs[0].pid} {procs[1].pid}"\n')
            text = (SCRIPTS / "init.sh").read_text()
            start = text.index("    # the USB Wi-Fi setup started as")
            end = text.index("    done\n", start) + len("    done\n")
            res = sb.run(text[start:end])
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(procs[0].wait(timeout=5), -9, "the fallback applet survived init.sh stop")
            time.sleep(0.2)
            self.assertIsNone(procs[1].poll(), "another hub_util process was killed")
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill()
                    p.wait()
            sb.cleanup()


class EmergencyBoot(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()
        sb = self.sb
        for name in ("killall", "reboot", "mount", "sleep", "sync", "load_modules", "udhcpc"):
            sb.recorder(name)
        harmony = sb.root / "data" / "harmony"
        (harmony / "reboot_counter").write_text("5\n")  # this boot is the sixth short one
        (harmony / "bt_disabled.conf").write_text("1\n")
        sb.device_file("data/harmony/init.sh", "exit 0\n")
        sb.device_file("data/harmony/bin/hub_util", f'echo "hub_util $*" >> \'{sb.calls}\'\n')
        sb.device_file("usr/bin/usbhid", f'echo "usbhid $*" >> \'{sb.calls}\'\n')

    def tearDown(self):
        self.sb.cleanup()

    def boot(self) -> list[str]:
        res = self.sb.run(self.sb.rewrite((SCRIPTS / "rcS.local").read_text()))
        self.assertTrue((self.sb.root / "tmp" / "emergency_recovery.log").exists(), res.stderr)
        time.sleep(0.3)  # both are started in the background
        return self.sb.call_lines()

    def test_emergency_boot_starts_usb_wifi_setup(self):
        calls = self.boot()
        self.assertIn("hub_util usbwifi", calls)
        self.assertIn("usbhid -s", calls)

    def test_emergency_dhcp_client_writes_its_pid_file(self):
        # hub_util usbwifi asks udhcpc for a new lease through this file after a good set
        calls = self.boot()
        dhcp = [c for c in calls if c.startswith("udhcpc ")]
        self.assertEqual(len(dhcp), 1, calls)
        self.assertIn("-i ath0", dhcp[0])
        self.assertRegex(dhcp[0], r"-p \S*/var/run/udhcpc\.ath0\.pid( |$)")

    def test_emergency_boot_respects_setting_off(self):
        (self.sb.root / "data" / "harmony" / "ethernet.conf").write_text("ETH_ENABLED=1\nUSB_WIFI_SETUP=0\n")
        calls = self.boot()
        self.assertNotIn("hub_util usbwifi", calls)
        self.assertNotIn("usbhid -s", calls)


if __name__ == "__main__":
    unittest.main(verbosity=2)
