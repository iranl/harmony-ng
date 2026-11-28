#!/usr/bin/env python3
"""build/kernel6/modprobe.sh: the Linux 6.18 Hub system's modprobe (and the kernel's on-demand
loader). A name resolves through modules.dep (dashes and underscores alike) or modules.alias, the
needs load first, and a name that is not there fails without loading anything."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "build" / "kernel6" / "modprobe.sh"

DEP = """kernel/net/wireless/cfg80211.ko:
kernel/net/mac80211/mac80211.ko: kernel/lib/crypto/libarc4.ko kernel/net/wireless/cfg80211.ko
kernel/lib/crypto/libarc4.ko:
kernel/drivers/bluetooth/hci_uart.ko: kernel/net/bluetooth/bluetooth.ko
kernel/net/bluetooth/bluetooth.ko:
"""
ALIAS = """alias tty-ldisc-15 hci_uart
alias net-pf-31 bluetooth
alias usb:v0B95p772Bd*dc*dsc*dp*ic*isc*ip*in* asix
"""


class ModprobeScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        r = pathlib.Path(self.tmp.name)
        self.mods = r / "modules"
        self.mods.mkdir()
        (self.mods / "modules.dep").write_text(DEP)
        (self.mods / "modules.alias").write_text(ALIAS)
        stub = r / "stub"
        stub.mkdir()
        self.log = r / "calls"
        (stub / "insmod").write_text(f"#!/bin/sh\necho insmod \"${{1##*/}}\" >> '{self.log}'\n")
        (stub / "insmod").chmod(0o755)
        # nothing loaded yet: an empty list in place of the host's /proc/modules
        loaded = r / "loaded"
        loaded.write_text("")
        self.env = dict(os.environ, PATH=f"{stub}:/usr/bin:/bin", MODPROBE_DIR=str(self.mods),
                        MODPROBE_LOADED=str(loaded))

    def tearDown(self):
        self.tmp.cleanup()

    def run_mp(self, *args):
        return subprocess.run(["sh", str(SCRIPT), *args], env=self.env, capture_output=True, text=True,
                              timeout=30)

    def calls(self):
        return self.log.read_text().split("\n")[:-1] if self.log.exists() else []

    def test_name_with_needs_first(self):
        self.assertEqual(self.run_mp("mac80211").returncode, 0)
        self.assertEqual(self.calls(), ["insmod cfg80211.ko", "insmod libarc4.ko", "insmod mac80211.ko"])

    def test_alias(self):
        self.assertEqual(self.run_mp("tty-ldisc-15").returncode, 0)
        self.assertEqual(self.calls(), ["insmod bluetooth.ko", "insmod hci_uart.ko"])

    def test_dash_and_underscore_alike(self):
        self.assertEqual(self.run_mp("hci-uart").returncode, 0)
        self.assertIn("insmod hci_uart.ko", self.calls())

    def test_missing_fails_fast(self):
        start = time.monotonic()
        res = self.run_mp("-q", "net-pf-10")
        self.assertEqual(res.returncode, 1)
        self.assertEqual(self.calls(), [])
        self.assertEqual(res.stderr, "")
        self.assertLess(time.monotonic() - start, 5)


if __name__ == "__main__":
    unittest.main()
