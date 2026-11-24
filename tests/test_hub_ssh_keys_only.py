#!/usr/bin/env python3
"""Every path that starts SSH on the Hub starts dropbear for key logins only (-s -g), and the Hub
never loses SSH because of it.

The scripts run with sh in a sandbox: device paths are moved under a temporary root, the server
binaries are stand-ins that write the arguments they got (and whether SIGHUP is ignored) to a
file, and the commands with side effects are stubs first in PATH. The boot script runs with a
PATH that holds only applets of the stock Hub BusyBox (no readlink, no cmp).

- payload/scripts/hub_sshd finds the binary in the documented order and passes -s -g -K 300 -R
  before the caller's own arguments; --binary only prints the binary; a script at
  /usr/sbin/dropbearmulti (the USB root tool's boot script) is never started; an empty host key
  file is removed before the server starts (it would exit on it)
- rcS.local copies it to /usr/sbin/dropbear (the stock boot script starts that path right after
  it), on a normal and an emergency boot, rewrites nothing on the next boot, and keeps SSH when the
  wrapper is missing, empty, or the old link dangles; an older build deployed over this one leaves
  a working /usr/sbin/dropbear
- init.sh copies it too and starts the server without its own ignored SIGHUP
- the USB root tool's boot script writes a key-only /usr/sbin/dropbear and starts the server with
  -s -g
"""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPTS = REPO_DIR / "payload" / "scripts"
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools import root_hub_usb  # noqa: E402

DEVICE_DIRS = "data|tmp|etc|root|home|usr|sbin|bin|cache|proc|sys|opt|lib"
STUBS = ("killall", "reboot", "mount", "sleep", "sync", "load_modules", "udhcpc", "wlanconfig")
# Applets of the stock Hub BusyBox (v1.13.4) that the boot script may use; readlink is not one
STOCK_APPLETS = ("sh", "cat", "chmod", "cp", "cut", "date", "dirname", "echo", "expr", "grep", "ln", "ls",
                 "mkdir", "mv", "rm", "sed", "touch", "test", "[")
# What dropbear before this change found at /data/harmony/dropbear (older releases ship it)
OLD_WRAPPER = '#!/bin/sh\nBIN=/data/harmony/bin/dropbear\nexec "$BIN" -s -g -K 300 "$@"\n'
# The dropbear block of init.sh before this change: it only acts when a separate binary exists
OLD_INIT_BLOCK = '''_dropbear_bin=""
[ -x /data/harmony/bin/dropbear ] && _dropbear_bin=/data/harmony/bin/dropbear
if [ -n "$_dropbear_bin" ]; then
  printf '#!/bin/sh\\nexec %s -K 300 "$@"\\n' "$_dropbear_bin" > /usr/sbin/dropbear
  chmod 755 /usr/sbin/dropbear
fi
'''


class Sandbox:
    def __init__(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.root = base / "root"
        self.stub = base / "stub"
        self.stub.mkdir()
        self.stock = base / "stockbin"
        self.stock.mkdir()
        for name in STOCK_APPLETS:
            path = shutil.which(name)
            if path:
                (self.stock / name).symlink_to(path)
        for d in ("data/harmony/bin", "tmp", "usr/sbin", "usr/bin", "etc", "cache/bin"):
            (self.root / d).mkdir(parents=True)
        for name in STUBS:
            self.write(self.stub / name, "#!/bin/sh\nexit 0\n")
        self.argv_file = base / "argv.txt"
        self.base = base
        self.usr_dropbear = self.root / "usr" / "sbin" / "dropbear"

    @staticmethod
    def write(path: pathlib.Path, text: str, mode: int = 0o755) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(mode)

    def sandboxed_text(self, text: str) -> str:
        return re.sub(r"(?<![\w./$!-])/(" + DEVICE_DIRS + r")/", lambda m: f"{self.root}/{m.group(1)}/", text)

    def fake_server(self, rel: str) -> None:
        """A stand-in server binary: records its name, arguments and ignored-signal mask"""
        self.write(self.root / rel,
                   f'#!/bin/sh\necho "$0 $*" >> "{self.argv_file}"\n'
                   f'grep SigIgn /proc/$$/status >> "{self.argv_file}.sig" 2>/dev/null\n')

    def install_wrapper(self, text: str | None = None) -> None:
        self.write(self.root / "data" / "harmony" / "hub_sshd",
                   self.sandboxed_text((SCRIPTS / "hub_sshd").read_text() if text is None else text))

    def run(self, script: pathlib.Path, *args: str, ps_out: str = "", stock_path: bool = False,
            prefix: str = "") -> subprocess.CompletedProcess[str]:
        self.write(self.stub / "ps", f"#!/bin/sh\necho '{ps_out}'\n")
        path = f"{self.stub}:{self.stock}" if stock_path else f"{self.stub}:/usr/bin:/bin"
        if prefix:
            cmd = ["sh", "-c", f'{prefix}\n. "{script}"', "sh", *args]
        else:
            cmd = ["sh", str(script), *args]
        return subprocess.run(cmd, env=dict(os.environ, PATH=path), capture_output=True, text=True, timeout=30)

    def started(self) -> list[str]:
        if not self.argv_file.exists():
            return []
        return [line.strip() for line in self.argv_file.read_text().splitlines() if line.strip()]

    def clear(self) -> None:
        for p in (self.argv_file, pathlib.Path(f"{self.argv_file}.sig")):
            if p.exists():
                p.unlink()

    def hup_ignored(self) -> bool:
        line = pathlib.Path(f"{self.argv_file}.sig").read_text().split()[-1]
        return bool(int(line, 16) & 1)

    def close(self) -> None:
        self.tmp.cleanup()


class Wrapper(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox()
        self.s.install_wrapper()
        self.wrapper = self.s.root / "data" / "harmony" / "hub_sshd"

    def tearDown(self):
        self.s.close()

    def test_multicall_binary_gets_keys_only_flags(self):
        self.s.fake_server("data/dropbearmulti")
        res = self.s.run(self.wrapper, "-p", "22")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g -K 300 -R -p 22"])

    def test_separate_binary_is_preferred_and_gets_the_flags(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.fake_server("data/harmony/bin/dropbear")
        self.s.run(self.wrapper)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/harmony/bin/dropbear -s -g -K 300 -R"])

    def test_update_package_binary_comes_next(self):
        self.s.fake_server("data/harmony/bin/dropbearmulti")
        self.s.run(self.wrapper)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/harmony/bin/dropbearmulti dropbear -s -g -K 300 -R"])

    def test_update_package_binary_preferred_over_usb_root_binary(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.fake_server("data/harmony/bin/dropbearmulti")
        res = self.s.run(self.wrapper, "--binary")
        self.assertEqual(res.stdout.strip(), f"{self.s.root}/data/harmony/bin/dropbearmulti")
        self.s.run(self.wrapper)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/harmony/bin/dropbearmulti dropbear -s -g -K 300 -R"])

    def test_serial_guide_binary_but_never_the_root_tools_boot_script(self):
        multi = self.s.root / "usr" / "sbin" / "dropbearmulti"
        self.s.write(multi, f'#!/bin/sh\necho "boot script ran" >> "{self.s.argv_file}"\n')
        res = self.s.run(self.wrapper)
        self.assertNotEqual(res.returncode, 0)
        self.assertEqual(self.s.started(), [], "the USB root tool's boot script is not a server")
        multi.write_bytes(b"\x7fELF\x01\x02\x01" + b"\x00" * 64)
        res = self.s.run(self.wrapper, "--binary")
        self.assertEqual(res.stdout.strip(), str(multi))

    def test_empty_binary_is_skipped(self):
        self.s.write(self.s.root / "data" / "dropbearmulti", "")
        self.s.fake_server("data/harmony/bin/dropbearmulti")
        res = self.s.run(self.wrapper, "--binary")
        self.assertEqual(res.stdout.strip(), f"{self.s.root}/data/harmony/bin/dropbearmulti")

    def test_binary_query_starts_nothing(self):
        self.s.fake_server("data/dropbearmulti")
        res = self.s.run(self.wrapper, "--binary")
        self.assertEqual(res.returncode, 0)
        self.assertEqual(res.stdout.strip(), f"{self.s.root}/data/dropbearmulti")
        self.assertEqual(self.s.started(), [])

    def test_no_binary_fails(self):
        self.assertNotEqual(self.s.run(self.wrapper).returncode, 0)
        self.assertNotEqual(self.s.run(self.wrapper, "--binary").returncode, 0)

    def test_empty_host_key_removed_before_start(self):
        """-R creates a missing host key, but the server exits on an empty file: the wrapper
        removes an empty one before it starts the server, keeps a good one, and --binary leaves
        both alone"""
        self.s.fake_server("data/dropbearmulti")
        keys = self.s.root / "etc" / "dropbear"
        empty = keys / "dropbear_ed25519_host_key"
        rsa_empty = keys / "dropbear_rsa_host_key"
        self.s.write(empty, "", 0o600)
        self.s.write(rsa_empty, "", 0o600)
        self.s.run(self.wrapper, "--binary")
        self.assertTrue(empty.exists() and rsa_empty.exists(), "--binary starts nothing and changes nothing")
        res = self.s.run(self.wrapper, stock_path=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertFalse(empty.exists())
        self.assertFalse(rsa_empty.exists())
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g -K 300 -R"])
        self.s.write(empty, "good key\n", 0o600)
        self.s.run(self.wrapper, stock_path=True)
        self.assertEqual(empty.read_text(), "good key\n")


class BootScripts(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox()
        h = self.s.root / "data" / "harmony"
        self.s.write(h / "bt_disabled.conf", "1\n", 0o644)
        self.s.write(h / "init.sh", "#!/bin/sh\nexit 0\n")
        self.usr = self.s.usr_dropbear

    def tearDown(self):
        self.s.close()

    def boot(self) -> None:
        out = self.s.base / "rcS.local"
        out.write_text(self.s.sandboxed_text((SCRIPTS / "rcS.local").read_text()))
        self.s.run(out, stock_path=True)

    def stock_start(self) -> list[str]:
        """What the stock /etc/init.d/rcS does after rcS.local"""
        self.s.clear()
        r = subprocess.run(["sh", "-c", f'[ -x "{self.usr}" ] && "{self.usr}"; true'],
                           env=dict(os.environ, PATH=f"{self.s.stub}:/usr/bin:/bin"), capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        return self.s.started()

    def keys_only(self) -> list[str]:
        return [f"{self.s.root}/data/dropbearmulti dropbear -s -g -K 300 -R"]

    def rooted_hub(self) -> None:
        """As the USB root tool left a Hub before this change: /usr/sbin/dropbear -> the binary"""
        self.s.fake_server("data/dropbearmulti")
        self.usr.symlink_to(self.s.root / "data" / "dropbearmulti")

    def test_normal_boot_copies_the_wrapper(self):
        self.rooted_hub()
        self.s.install_wrapper()
        self.boot()
        self.assertFalse(self.usr.is_symlink())
        self.assertEqual(self.usr.read_text(), (self.s.root / "data" / "harmony" / "hub_sshd").read_text())
        self.assertEqual(self.stock_start(), self.keys_only())

    def test_second_boot_writes_nothing(self):
        self.rooted_hub()
        self.s.install_wrapper()
        self.boot()
        before = (os.lstat(self.usr).st_ino, os.lstat(self.usr).st_mtime_ns)
        time.sleep(0.05)
        self.boot()
        self.assertEqual((os.lstat(self.usr).st_ino, os.lstat(self.usr).st_mtime_ns), before)

    def test_older_build_deployed_over_it(self):
        self.rooted_hub()
        self.s.install_wrapper()
        self.boot()
        # an older bundle: its scripts/dropbear lands in /data/harmony, its init.sh block runs
        self.s.write(self.s.root / "data" / "harmony" / "dropbear", self.s.sandboxed_text(OLD_WRAPPER))
        old = self.s.base / "old_init_block.sh"
        old.write_text(self.s.sandboxed_text(OLD_INIT_BLOCK))
        self.s.run(old)
        self.assertEqual(self.stock_start(), self.keys_only())

    def test_dangling_link_and_no_wrapper_keep_ssh(self):
        self.s.fake_server("data/dropbearmulti")
        self.usr.symlink_to(self.s.root / "data" / "harmony" / "dropbear")  # target missing
        self.boot()
        self.assertEqual(self.stock_start(), self.keys_only())

    def test_empty_wrapper_is_not_installed(self):
        self.rooted_hub()
        self.s.install_wrapper("")
        self.boot()
        self.assertTrue(self.usr.is_symlink(), "/usr/sbin/dropbear left as it was")
        self.assertEqual(self.stock_start(), [str(self.usr)], "SSH still starts (as before, through the link)")

    def test_emergency_boot_starts_keys_only(self):
        self.rooted_hub()
        self.s.install_wrapper()
        (self.s.root / "data" / "harmony" / "reboot_counter").write_text("5\n")
        self.boot()
        self.assertTrue((self.s.root / "tmp" / "emergency_recovery.log").exists())
        self.assertEqual(self.s.started(), self.keys_only())

    def test_emergency_boot_without_usable_dropbear_file(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.write(self.usr, "")  # empty file, no wrapper to copy
        (self.s.root / "data" / "harmony" / "reboot_counter").write_text("5\n")
        self.boot()
        self.assertTrue((self.s.root / "tmp" / "emergency_recovery.log").exists())
        self.assertEqual(self.s.started(), self.keys_only())

    def init_block(self) -> pathlib.Path:
        text = (SCRIPTS / "init.sh").read_text()
        helpers = text[text.index("write_if_changed() {"): text.index('echo "$(date) harmony init start')]
        body = text[text.index("# 3. Shell Profile"): text.index("# 4. Disable")]
        out = self.s.base / "init_block.sh"
        out.write_text(self.s.sandboxed_text(helpers + body))
        return out

    def test_init_copies_and_starts_without_ignored_hup(self):
        self.rooted_hub()
        self.s.install_wrapper()
        self.s.run(self.init_block(), ps_out="  1 root init", prefix="trap '' HUP")
        self.assertFalse(self.usr.is_symlink())
        self.assertEqual(self.s.started(), self.keys_only())
        self.assertFalse(self.s.hup_ignored(), "SSH sessions must not inherit init.sh's ignored SIGHUP")

    def test_init_starts_the_wrapper_when_dropbear_cannot_be_written(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.install_wrapper()
        sbin = self.s.root / "usr" / "sbin"
        sbin.chmod(0o555)  # /usr/sbin/dropbear cannot be written
        try:
            self.s.run(self.init_block(), ps_out="  1 root init")
        finally:
            sbin.chmod(0o755)
        self.assertFalse(self.usr.exists())
        self.assertEqual(self.s.started(), self.keys_only())

    def test_init_does_not_start_a_second_server(self):
        self.rooted_hub()
        self.s.install_wrapper()
        self.s.run(self.init_block(), ps_out="  7 root /data/dropbearmulti dropbear")
        self.assertEqual(self.s.started(), [])

    def test_rcs_local_fallback_prefers_harmony_bin(self):
        self.s.fake_server("data/dropbearmulti")
        self.s.fake_server("data/harmony/bin/dropbearmulti")
        self.boot()
        self.assertTrue(self.usr.exists())
        self.assertIn("/data/harmony/bin/dropbearmulti", self.usr.read_text())
        self.assertEqual(self.stock_start(), [f"{self.s.root}/data/harmony/bin/dropbearmulti dropbear -s -g -K 300 -R"])

    def test_rcs_local_fallback_uses_data_dropbearmulti_when_harmony_bin_absent(self):
        self.s.fake_server("data/dropbearmulti")
        self.boot()
        self.assertTrue(self.usr.exists())
        self.assertIn("/data/dropbearmulti", self.usr.read_text())
        self.assertEqual(self.stock_start(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g -K 300 -R"])


class RootToolBootScript(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox()
        self.s.write(self.s.stub / "uname", "#!/bin/sh\necho mips\n")
        self.s.write(self.s.root / "etc" / "passwd", "root:x:0:0::/root:/bin/sh\n", 0o644)
        # a host key exists, so the script does not run dropbearkey (the stand-in would record it)
        self.s.write(self.s.root / "etc" / "dropbear" / "dropbear_ed25519_host_key", "key\n", 0o600)
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
        usr_dropbear = self.s.usr_dropbear
        self.assertFalse(usr_dropbear.is_symlink())
        self.assertEqual(usr_dropbear.stat().st_mode & 0o777, 0o755)
        self.s.clear()
        self.s.run(usr_dropbear)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbear -s -g"])


class DropbearkeyWrapper(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox()
        self.script = self.s.base / "dropbearkey"
        self.script.write_text(self.s.sandboxed_text((SCRIPTS / "dropbearkey").read_text()))

    def tearDown(self):
        self.s.close()

    def test_prefers_harmony_bin_dropbearkey(self):
        self.s.fake_server("data/harmony/bin/dropbearkey")
        self.s.fake_server("data/harmony/bin/dropbearmulti")
        self.s.fake_server("data/dropbearmulti")
        res = self.s.run(self.script, "-t", "ed25519")
        self.assertEqual(res.returncode, 0)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/harmony/bin/dropbearkey -t ed25519"])

    def test_falls_back_to_harmony_bin_dropbearmulti(self):
        self.s.fake_server("data/harmony/bin/dropbearmulti")
        self.s.fake_server("data/dropbearmulti")
        res = self.s.run(self.script, "-t", "ed25519")
        self.assertEqual(res.returncode, 0)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/harmony/bin/dropbearmulti dropbearkey -t ed25519"])

    def test_falls_back_to_data_dropbearmulti(self):
        self.s.fake_server("data/dropbearmulti")
        res = self.s.run(self.script, "-t", "ed25519")
        self.assertEqual(res.returncode, 0)
        self.assertEqual(self.s.started(), [f"{self.s.root}/data/dropbearmulti dropbearkey -t ed25519"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
