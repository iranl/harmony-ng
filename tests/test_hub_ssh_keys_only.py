#!/usr/bin/env python3
"""Every path that starts SSH on the Hub starts dropbear for key logins only (-s -g).

The scripts run with sh in a sandbox: device paths are moved under a temporary root, the server
binaries are stand-ins that write the arguments they got to a file, and the commands with side
effects are stubs first in PATH.

- payload/scripts/dropbear (the wrapper) finds the binary in the documented order and passes
  -s -g -K 300 before the caller's own arguments; --binary only prints the binary
- rcS.local links /usr/sbin/dropbear to the wrapper (the stock boot script starts that path right
  after it), on a normal and on an emergency boot; with no server binary it leaves the old one
- init.sh links it too and starts the server when none runs
- the USB root tool's boot script writes a key-only /usr/sbin/dropbear and starts the server
  with -s -g
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPTS = REPO_DIR / "payload" / "scripts"
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools import root_hub_usb  # noqa: E402

DEVICE_DIRS = "data|tmp|etc|root|home|usr|sbin|bin|cache|proc|sys|opt|lib"
STUBS = ("killall", "reboot", "mount", "sleep", "sync", "load_modules", "udhcpc", "wlanconfig")


class Sandbox:
    def __init__(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.root = base / "root"
        self.stub = base / "stub"
        self.stub.mkdir()
        for d in ("data/harmony/bin", "tmp", "usr/sbin", "usr/bin", "etc", "cache/bin"):
            (self.root / d).mkdir(parents=True)
        for name in STUBS:
            self.write(self.stub / name, "#!/bin/sh\nexit 0\n")
        self.argv_file = base / "argv.txt"
        self.base = base

    @staticmethod
    def write(path: pathlib.Path, text: str, mode: int = 0o755) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(mode)

    def sandboxed_text(self, text: str) -> str:
        return re.sub(r"(?<![\w./$!-])/(" + DEVICE_DIRS + r")/", lambda m: f"{self.root}/{m.group(1)}/", text)

    def fake_server(self, rel: str) -> None:
        """A stand-in server binary that records its name and arguments"""
        self.write(self.root / rel, f'#!/bin/sh\necho "$0 $*" >> "{self.argv_file}"\n')

    def install_wrapper(self) -> None:
        self.write(self.root / "data" / "harmony" / "dropbear",
                   self.sandboxed_text((SCRIPTS / "dropbear").read_text()))

    def run(self, script: pathlib.Path, *args: str, ps_out: str = "") -> subprocess.CompletedProcess[str]:
        self.write(self.stub / "ps", f"#!/bin/sh\necho '{ps_out}'\n")
        env = dict(os.environ, PATH=f"{self.stub}:/usr/bin:/bin")
        return subprocess.run(["sh", str(script), *args], env=env, capture_output=True, text=True, timeout=30)

    def started(self) -> list[str]:
        if not self.argv_file.exists():
            return []
        return [line.strip() for line in self.argv_file.read_text().splitlines() if line.strip()]

    def close(self) -> None:
        self.tmp.cleanup()


class Wrapper(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox()
        self.s.install_wrapper()
        self.wrapper = self.s.root / "data" / "harmony" / "dropbear"

    def tearDown(self):
        self.s.close()

    def test_multicall_binary_gets_keys_only_flags(self):
        self.s.fake_server("data/dropbearmulti")
        res = self.s.run(self.wrapper, "-p", "22")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g -K 300 -p 22"])

    def test_separate_binary_is_preferred_and_gets_the_flags(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.fake_server("data/harmony/bin/dropbear")
        self.s.run(self.wrapper)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/harmony/bin/dropbear -s -g -K 300"])

    def test_update_package_binary_is_the_last_choice(self):
        self.s.fake_server("data/harmony/bin/dropbearmulti")
        self.s.run(self.wrapper)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/harmony/bin/dropbearmulti dropbear -s -g -K 300"])

    def test_binary_query_starts_nothing(self):
        self.s.fake_server("data/dropbearmulti")
        res = self.s.run(self.wrapper, "--binary")
        self.assertEqual(res.returncode, 0)
        self.assertEqual(res.stdout.strip(), f"{self.s.root}/data/dropbearmulti")
        self.assertEqual(self.s.started(), [])

    def test_no_binary_fails(self):
        res = self.s.run(self.wrapper)
        self.assertNotEqual(res.returncode, 0)
        res = self.s.run(self.wrapper, "--binary")
        self.assertNotEqual(res.returncode, 0)


class BootScripts(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox()
        h = self.s.root / "data" / "harmony"
        self.s.write(h / "bt_disabled.conf", "1\n", 0o644)
        self.s.write(h / "init.sh", "#!/bin/sh\nexit 0\n")
        self.usr_dropbear = self.s.root / "usr" / "sbin" / "dropbear"

    def tearDown(self):
        self.s.close()

    def rcs_local(self) -> pathlib.Path:
        out = self.s.base / "rcS.local"
        out.write_text(self.s.sandboxed_text((SCRIPTS / "rcS.local").read_text()))
        return out

    def stock_start(self) -> None:
        """What the stock /etc/init.d/rcS does after rcS.local: run /usr/sbin/dropbear"""
        self.s.run(self.usr_dropbear)

    def test_normal_boot_links_the_wrapper(self):
        # as the USB root tool left it: /usr/sbin/dropbear -> the multi-call binary
        self.s.fake_server("data/dropbearmulti")
        self.usr_dropbear.symlink_to(self.s.root / "data" / "dropbearmulti")
        self.s.install_wrapper()
        self.s.run(self.rcs_local())
        self.assertEqual(os.readlink(self.usr_dropbear), f"{self.s.root}/data/harmony/dropbear")
        self.stock_start()
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g -K 300"])

    def test_emergency_boot_starts_keys_only(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.install_wrapper()
        (self.s.root / "data" / "harmony" / "reboot_counter").write_text("5\n")
        self.s.run(self.rcs_local())
        self.assertTrue((self.s.root / "tmp" / "emergency_recovery.log").exists())
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g -K 300"])

    def test_no_server_binary_leaves_dropbear_alone(self):
        self.s.write(self.usr_dropbear, "#!/bin/sh\n# something else\n")
        self.s.install_wrapper()
        self.s.run(self.rcs_local())
        self.assertFalse(self.usr_dropbear.is_symlink())
        self.assertIn("something else", self.usr_dropbear.read_text())

    def init_block(self) -> pathlib.Path:
        text = (SCRIPTS / "init.sh").read_text()
        helpers = text[text.index("write_if_changed() {"): text.index('echo "$(date) harmony init start')]
        body = text[text.index("# 3. Shell Profile"): text.index("# 4. Disable")]
        out = self.s.base / "init_block.sh"
        out.write_text(self.s.sandboxed_text(helpers + body))
        return out

    def test_init_links_and_starts_when_none_runs(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.install_wrapper()
        self.s.run(self.init_block(), ps_out="  1 root init")
        self.assertEqual(os.readlink(self.usr_dropbear), f"{self.s.root}/data/harmony/dropbear")
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g -K 300"])

    def test_init_does_not_start_a_second_server(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.install_wrapper()
        self.s.run(self.init_block(), ps_out="  7 root /data/dropbearmulti dropbear")
        self.assertEqual(self.s.started(), [])


class RootToolBootScript(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox()
        self.s.write(self.s.stub / "uname", "#!/bin/sh\necho mips\n")
        self.s.write(self.s.root / "etc" / "passwd", "root:x:0:0::/root:/bin/sh\n", 0o644)
        # a host key exists, so the script does not run dropbearkey (the stand-in would record it)
        self.s.write(self.s.root / "etc" / "dropbear" / "dropbear_rsa_host_key", "key\n", 0o600)
        self.s.fake_server("data/dropbearmulti")

    def tearDown(self):
        self.s.close()

    def test_boot_script_is_keys_only(self):
        text = root_hub_usb.BOOTSTRAP_SCRIPT_TEMPLATE.decode("ascii")
        text = text.replace("%PUBKEY%", "ssh-ed25519 AAAA test").replace("%OFFSET%", "00000000")
        script = self.s.base / "boot.sh"
        script.write_text(self.s.sandboxed_text(text))
        self.s.run(script)
        # the server it starts now
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g -R -p 0.0.0.0:22"])
        # what the stock boot script starts at every later boot
        usr_dropbear = self.s.root / "usr" / "sbin" / "dropbear"
        self.assertFalse(usr_dropbear.is_symlink())
        self.assertEqual(usr_dropbear.stat().st_mode & 0o777, 0o755)
        self.s.argv_file.unlink()
        self.s.run(usr_dropbear)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
