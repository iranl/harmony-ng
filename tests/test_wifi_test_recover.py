#!/usr/bin/env python3
"""The Wi-Fi test in hub_daemon (web UI path, wifi_config.c shared with the USB setup).

hub_daemon_test runs with HARMONY_WIFI_ROOT on a temporary root: the wpa_supplicant files,
wpa_cli (a script, see test_usb_wifi.py) and the test lock live there. Checks:
- a Wi-Fi test cut short (its mark and copies left on flash, the candidate in both files, the
  lock file of a dead process) is undone when hub_daemon starts: both files back, copies gone;
- POST /api/wifi-test with a wrong password on a Hub with no Wi-Fi file before: 400 with the
  reason, and neither file exists afterwards (the candidate, password included, is removed).
"""

import fcntl
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_mock_lib  # noqa: E402
from test_usb_wifi import FAKE_SUPPLICANT, FAKE_WPA_CLI, OLD_CONF  # noqa: E402

PORT = 9175
CANDIDATE = 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n\nnetwork={\n\tssid="FarAway"\n\tpsk="unreachable1"\n}\n'


def make_root(base: pathlib.Path) -> pathlib.Path:
    for d in ("etc", "tmp", "usr/sbin", "var/run"):
        (base / d).mkdir(parents=True, exist_ok=True)
    for name, text in (("wpa_cli", FAKE_WPA_CLI), ("wpa_supplicant", FAKE_SUPPLICANT)):
        p = base / "usr" / "sbin" / name
        p.write_text(text.replace("@ROOT@", str(base)))
        p.chmod(0o755)
    return base


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + str(detail)))
        if not cond:
            failures.append(name)

    build_mock_lib()
    with tempfile.TemporaryDirectory() as d:
        root = make_root(pathlib.Path(d))
        etc = root / "etc"
        conf, ath0 = etc / "wpa_supplicant.conf", etc / "wpa_supplicant.ath0.conf"
        # what a test killed in the middle leaves
        conf.write_text(CANDIDATE)
        ath0.write_text(CANDIDATE)
        (etc / "wpa_supplicant.conf.pretest").write_text(OLD_CONF)
        (etc / "wpa_supplicant.ath0.conf.pretest").write_text(OLD_CONF)
        (etc / "wpa_supplicant.pretest").write_text("conf=1\nath0=1\n")
        dead = subprocess.Popen(["true"])
        dead.wait()
        (root / "tmp" / "harmony_wifi_test.lock").write_text(f"{dead.pid}\n")
        env = {"HARMONY_WIFI_ROOT": str(root), "HARMONY_WIFI_TEST_WAIT": "2"}
        with HubHarness(port=PORT, env=env) as hub:
            check(conf.read_text() == OLD_CONF and ath0.read_text() == OLD_CONF,
                  "hub_daemon start puts the previous Wi-Fi files back", conf.read_text())
            check(not any("pretest" in p.name for p in etc.iterdir()), "copies and mark removed")
            with open(root / "tmp" / "harmony_wifi_test.lock", "a+") as lk:
                try:
                    fcntl.flock(lk, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    free = True
                except BlockingIOError:
                    free = False
            check(free, "lock released after the recovery")

            conf.unlink()
            ath0.unlink()
            st, r = hub.http_post("/api/wifi-test", {"ssid": "NewNet", "password": "wrongpassword"}, timeout=30)
            check(st == 400 and "Incorrect Wi-Fi password" in str(r), "wrong password refused with the reason", (st, r))
            check(not conf.exists() and not ath0.exists(),
                  "no Wi-Fi file before: the failed candidate is removed again",
                  [p.name for p in etc.iterdir()])

    if failures:
        print("Wi-Fi test recovery: %d FAILED" % len(failures))
        return 1
    print("all Wi-Fi test recovery checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
