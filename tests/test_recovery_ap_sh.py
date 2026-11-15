#!/usr/bin/env python3
"""payload/scripts/recovery_ap.sh on the host, with the radio commands replaced by stubs.

- /data/harmony/recovery_ap.conf is read as KEY=VALUE data: shell code in it is never run,
  a valid pass phrase and WPA_ENABLED are taken, an invalid pass phrase keeps the default;
- the Wi-Fi portal is started again when it ends while the AP is up, and stays stopped after
  "stop" (before, a portal that died left nobody able to enter Wi-Fi values).
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "recovery_ap.sh"


class RecoveryApScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for d in ("tmp", "data/harmony/bin", "run", "stub"):
            (self.root / d).mkdir(parents=True)
        text = SCRIPT.read_text()
        text = text.replace("/tmp/", f"{self.root}/tmp/")
        text = text.replace("/data/harmony", f"{self.root}/data/harmony")
        text = text.replace("/var/run/", f"{self.root}/run/")
        text = text.replace("/sys/", f"{self.root}/sys/")
        self.text = text
        self.script = self.root / "recovery_ap.sh"
        self.script.write_text(text)
        self.conf = self.root / "data" / "harmony" / "recovery_ap.conf"
        self.marker = self.root / "injected"

    def tearDown(self):
        pid_file = self.root / "run" / "hub-recovery-ap.pid"
        if pid_file.exists():
            pid_file.unlink()
        self.kill_portal()
        self.tmp.cleanup()

    def kill_portal(self):
        pf = self.root / "portal.pid"
        if pf.exists():
            try:
                os.kill(int(pf.read_text().strip()), 9)
            except (ValueError, ProcessLookupError):
                pass

    def config_values(self) -> str:
        head = self.text[: self.text.index("stop_station()")]
        res = subprocess.run(["sh", "-c", head + 'echo "P=$WPA_PASSPHRASE E=$WPA_ENABLED"\n'],
                             capture_output=True, text=True, timeout=30)
        return res.stdout.strip()

    def test_conf_is_data_not_code(self):
        self.conf.write_text(f'WPA_PASSPHRASE="x"; touch {self.marker}; echo "\n'
                             f'WPA_ENABLED=$(touch {self.marker})\n')
        out = self.config_values()
        self.assertFalse(self.marker.exists(), "shell code in recovery_ap.conf was run")
        self.assertTrue(out.endswith(" E=1"), out)

    def test_conf_values_taken(self):
        self.conf.write_text('WPA_PASSPHRASE="my own pass 42"\nWPA_ENABLED=0\n')
        self.assertEqual(self.config_values(), "P=my own pass 42 E=0")
        self.conf.write_text("WPA_PASSPHRASE=short\n")
        self.assertEqual(self.config_values(), "P=harmonyrecovery E=1")

    def test_portal_restarted_until_stop(self):
        stub = self.root / "stub"
        for cmd in ("wlanconfig", "iwpriv", "iwconfig", "ifconfig"):
            (stub / cmd).write_text("#!/bin/sh\nexit 0\n")
        # killall: kills the stub portal (the real one matches hub_portal by name)
        (stub / "killall").write_text(
            f"#!/bin/sh\ncase \"$*\" in *hub_portal*) [ -f '{self.root}/portal.pid' ] && kill -9 $(cat '{self.root}/portal.pid') 2>/dev/null ;; esac\nexit 0\n")
        count = self.root / "portal_runs"
        portal = self.root / "data" / "harmony" / "bin" / "hub_portal"
        # ends at once twice (as a crashed portal would), then keeps running
        portal.write_text(
            f"#!/bin/sh\necho run >> '{count}'\necho $$ > '{self.root}/portal.pid'\n"
            f"[ $(wc -l < '{count}') -ge 3 ] && exec sleep 60\nexit 1\n")
        for f in list(stub.iterdir()) + [portal]:
            f.chmod(0o755)
        env = dict(os.environ, PATH=f"{stub}:/usr/bin:/bin")
        subprocess.run(["sh", str(self.script), "start"], env=env, timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 15
        while time.time() < deadline and (not count.exists() or len(count.read_text().split()) < 3):
            time.sleep(0.2)
        self.assertGreaterEqual(len(count.read_text().split()), 3, "the portal was not started again")

        subprocess.run(["sh", str(self.script), "stop"], env=env, timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        runs = len(count.read_text().split())
        time.sleep(3)
        self.assertEqual(len(count.read_text().split()), runs, "the portal came back after stop")


if __name__ == "__main__":
    unittest.main()
