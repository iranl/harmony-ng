#!/usr/bin/env python3
"""Tests for payload/scripts/init.sh section 7 (BTstack supervisor & Bluetooth re-attach).

Verifies that if hciattach dies or hci0 is missing, the supervisor automatically
terminates any stale hub_btstack process, reloads PSKeys, and re-attaches the radio.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
INIT = REPO_DIR / "payload" / "scripts" / "init.sh"
DEVICE_DIRS = "data|tmp|lib|sys|proc|usr|dev"


def bt_block() -> str:
    text = INIT.read_text(encoding="utf-8")
    start = text.index("# 7. BTstack Bluetooth Engine")
    end = text.index("# 8. CC2544 RF Daemon", start)
    block = text[start:end]
    block = block.replace(') &\necho "$!" >> /tmp/harmony_supervisors.pid', ')\n')
    return block


class TestInitBtReattach(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmpdir.name)
        self.root = self.base / "root"
        self.stub = self.base / "stub"
        self.calls = self.base / "calls.log"

        for d in ("data/harmony/bin", "tmp", "usr/sbin", "lib/firmware", "sys/class/net/ath0", "proc", "dev"):
            (self.root / d).mkdir(parents=True, exist_ok=True)
        self.stub.mkdir(parents=True, exist_ok=True)

        self.env = dict(os.environ, PATH=f"{self.stub.as_posix()}:/usr/sbin:/usr/bin:/bin")

        (self.root / "proc" / "mtd").write_text('mtd6: 00010000 00010000 "mfg"\n')
        (self.root / "dev" / "mtd6").write_text('hw_ver\n\n\n\n01.00\n')
        (self.root / "sys" / "class" / "net" / "ath0" / "address").write_text("00:11:22:33:44:55\n")
        (self.root / "lib" / "firmware" / "csr8811-a08.psr").write_text("&0x01ea = 0x0001 0xc200\n")
        (self.root / "proc" / "uptime").write_text("300.12 250.00\n")

        # Fake hub_btstack that terminates loop by creating harmony_stop
        hub_btstack = self.root / "data" / "harmony" / "bin" / "hub_btstack"
        hub_btstack.write_text(
            f"#!/bin/sh\necho \"hub_btstack\" >> \"{self.calls.as_posix()}\"\ntouch \"{self.root.as_posix()}/tmp/harmony_stop\"\nexit 0\n"
        )
        hub_btstack.chmod(0o755)

        for name in ("killall", "sleep"):
            self.make_recorder(name)

    def tearDown(self):
        self.tmpdir.cleanup()

    def make_stub(self, name: str, body: str):
        p = self.stub / name
        p.write_text(f"#!/bin/sh\n{body}\n")
        p.chmod(0o755)

    def make_recorder(self, name: str, extra: str = "", exit_code: int = 0):
        self.make_stub(name, f'echo "{name} $*" >> "{self.calls.as_posix()}"\n{extra}\nexit {exit_code}')

    def run_supervisor(self, text_override: str | None = None) -> subprocess.CompletedProcess:
        block = text_override or bt_block()
        # Single regex substitution to rewrite root directories without nesting
        block = re.sub(
            r"(?<![\w./$-])/(" + DEVICE_DIRS + r")/",
            lambda m: f"{self.root.as_posix()}/{m.group(1)}/",
            block
        )
        script = f'LOG="{self.root.as_posix()}/tmp/harmony-init.log"\n' + block
        return subprocess.run(["sh", "-c", script], env=self.env, capture_output=True, text=True, timeout=5)

    def test_radio_healthy_skips_reattach(self):
        """When hciattach is running and hci0 is UP, no re-attach occurs."""
        (self.root / "tmp" / "pskeys.psr").write_text("PSKEYS_EXIST\n")
        self.make_stub("pidof", 'if [ "$1" = "hciattach" ]; then exit 0; fi; exit 1')
        self.make_stub("hciconfig", 'echo "hci0: UP RUNNING RAW"; exit 0')
        self.make_recorder("bccmd")
        self.make_recorder("hciattach")

        res = self.run_supervisor()
        self.assertEqual(res.returncode, 0, res.stderr)

        calls = self.calls.read_text() if self.calls.exists() else ""
        self.assertNotIn("hciattach ttyS0", calls)
        self.assertNotIn("bccmd", calls)
        self.assertNotIn("killall -9 hciattach", calls)
        self.assertIn("hub_btstack", calls)

    def test_hciattach_dead_triggers_reattach(self):
        """When hciattach is dead, supervisor stops hub_btstack, runs bccmd and re-attaches."""
        (self.root / "tmp" / "pskeys.psr").write_text("PSKEYS_EXIST\n")
        self.make_stub("pidof", "exit 1")
        # A freshly attached hci0 is DOWN until hciconfig hci0 up
        self.make_stub(
            "hciconfig",
            f'echo "hciconfig $*" >> "{self.calls.as_posix()}"\n'
            f'if [ -f "{self.root.as_posix()}/tmp/attached" ]; then echo "hci0: DOWN"; exit 0; fi; exit 1'
        )
        self.make_recorder("bccmd")
        self.make_recorder("hciattach", f'touch "{self.root.as_posix()}/tmp/attached"')

        res = self.run_supervisor()
        self.assertEqual(res.returncode, 0, res.stderr)

        calls = self.calls.read_text() if self.calls.exists() else ""
        self.assertIn("killall -9 hub_btstack", calls)
        self.assertIn("killall -9 hciattach", calls)
        # The key file exists, so the boot set-up ran and the chip talks at 115200
        self.assertIn("bccmd -t bcsp -b 115200 psload -r", calls)
        self.assertIn("hciconfig hci0 sspmode 1", calls)
        self.assertIn("hciattach ttyS0 bcsp", calls)
        self.assertIn("hub_btstack", calls)

    def test_pskeys_generated_if_missing(self):
        """When /tmp/pskeys.psr is missing, supervisor builds it before bccmd."""
        pskeys_file = self.root / "tmp" / "pskeys.psr"
        self.assertFalse(pskeys_file.exists())

        self.make_stub("pidof", "exit 1")
        self.make_stub(
            "hciconfig",
            f'if [ -f "{self.root.as_posix()}/tmp/attached" ]; then echo "hci0: UP RUNNING"; exit 0; fi; exit 1'
        )
        self.make_stub(
            "bccmd",
            f'case "$*" in *buildname*) echo "CSR8811 A08"; exit 0 ;; esac\n'
            f'echo "bccmd $*" >> "{self.calls.as_posix()}"\nexit 0'
        )
        self.make_recorder("hciattach", f'touch "{self.root.as_posix()}/tmp/attached"')

        res = self.run_supervisor()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertTrue(pskeys_file.exists())
        self.assertIn("&0x0001 = 0x33 0x4455 0x22 0x0011\n", pskeys_file.read_text())
        calls = self.calls.read_text()
        # buildname answered at 38400: the chip is in its power-on state
        self.assertIn("bccmd -t bcsp psload -r", calls)
        self.assertNotIn("-b 115200 psload", calls)

    def test_hciattach_alive_hci0_missing(self):
        """hciattach runs but hci0 is gone: re-attach as well."""
        (self.root / "tmp" / "pskeys.psr").write_text("PSKEYS_EXIST\n")
        self.make_stub("pidof", 'if [ "$1" = "hciattach" ]; then exit 0; fi; exit 1')
        self.make_stub(
            "hciconfig",
            f'echo "hciconfig $*" >> "{self.calls.as_posix()}"\n'
            f'if [ -f "{self.root.as_posix()}/tmp/attached" ]; then echo "hci0: UP RUNNING"; exit 0; fi; exit 1'
        )
        self.make_recorder("bccmd")
        self.make_recorder("hciattach", f'touch "{self.root.as_posix()}/tmp/attached"')

        res = self.run_supervisor()
        self.assertEqual(res.returncode, 0, res.stderr)
        calls = self.calls.read_text()
        self.assertIn("killall -9 hciattach", calls)
        self.assertIn("hciattach ttyS0 bcsp", calls)

    def test_boot_setup_left_alone(self):
        """In the first minute of uptime, or while bccmd runs, rcS.local owns the radio."""
        for uptime, bccmd_running in (("12.50 10.00\n", False), ("300.00 250.00\n", True)):
            with self.subTest(uptime=uptime, bccmd=bccmd_running):
                self.calls.unlink(missing_ok=True)
                (self.root / "tmp" / "harmony_stop").unlink(missing_ok=True)
                (self.root / "proc" / "uptime").write_text(uptime)
                ok = "bccmd" if bccmd_running else "none"
                self.make_stub("pidof", f'[ "$1" = "{ok}" ] && exit 0; exit 1')
                self.make_stub("hciconfig", "exit 1")
                self.make_recorder("bccmd")
                self.make_recorder("hciattach")

                res = self.run_supervisor()
                self.assertEqual(res.returncode, 0, res.stderr)
                calls = self.calls.read_text()
                self.assertNotIn("killall -9 hciattach", calls)
                self.assertNotIn("bccmd", calls)
                self.assertIn("hub_btstack", calls)

    def test_failed_attach_backs_off(self):
        """hciattach does not bring hci0: no hub_btstack start, back-off, retry."""
        (self.root / "tmp" / "pskeys.psr").write_text("PSKEYS_EXIST\n")
        self.make_stub("pidof", "exit 1")
        self.make_stub("hciconfig", "exit 1")
        self.make_recorder("bccmd")
        self.make_recorder("hciattach", exit_code=1)
        # The back-off sleep ends the loop
        self.make_recorder("sleep", f'[ "$1" = "5" ] && touch "{self.root.as_posix()}/tmp/harmony_stop"')

        res = self.run_supervisor()
        self.assertEqual(res.returncode, 0, res.stderr)
        calls = self.calls.read_text()
        self.assertIn("hciattach ttyS0 bcsp", calls)
        self.assertNotIn("\nhub_btstack\n", calls)
        self.assertIn("did not attach, retry in 5s", (self.root / "tmp" / "harmony-init.log").read_text())


if __name__ == "__main__":
    unittest.main()
