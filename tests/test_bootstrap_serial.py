#!/usr/bin/env python3
"""remote/bootstrap_serial.py against a fake serial console.

The fake console runs each line the tool sends with sh, in a scratch folder that stands
for the remote's root (/data, /etc, /tmp, /usr, /home and /root moved under it), with a
PATH that has no base64 and no uudecode, as on the stock firmware. A fake /usr/bin/lua
decodes base64 like the stock Lua running the tool's decoder script; the native decoder
(an ARM program) is replaced by a host one. No serial port, no device.

- Without Lua the bootstrap must stop with an error and leave the boot script alone (the
  old tool emptied it and reported success).
- With Lua every file arrives intact, the key is in all three places, and dropbear starts
  without -B.
- A line lost on the way is caught by the MD5 check: nothing is replaced.
- Without --ssh-pubkey the owner key (~/.ssh/harmony_owner_*.pub) is installed: the stock
  root password is unknown, so Dropbear without a key would let nobody in. Without any key
  the bootstrap sends nothing.
"""

from __future__ import annotations

import base64
import contextlib
import importlib
import io
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_DIR / "remote"))

HOST_DECODER = """#!/usr/bin/env python3
import base64, sys
data = open(sys.argv[-2], "rb").read()
open(sys.argv[-1], "wb").write(base64.b64decode(b"".join(data.split())))
"""
SHELL_TOOLS = ("rm", "md5sum", "chmod", "mv", "mkdir", "ln", "cp", "cat", "sed")


class FakeConsole:
    def __init__(self, root: pathlib.Path, path_dir: pathlib.Path, host_decoder: pathlib.Path, drop_line: int | None):
        self.root, self.path_dir, self.host_decoder = root, path_dir, host_decoder
        self.pending = b""
        self.out = b""
        self.timeout = 1.0
        self.lines: list[str] = []
        self.drop_line = drop_line
        self.echo_count = 0

    def map(self, cmd: str) -> str:
        cmd = re.sub(r"(?<![\w./-])/(data|etc|tmp|usr|home|root)/", lambda m: f"{self.root}/{m.group(1)}/", cmd)
        return cmd.replace(f"{self.root}/tmp/b64dec_tiny {self.root}/", f"{self.host_decoder} {self.root}/")

    def write(self, data: bytes) -> int:
        self.pending += data
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            cmd = line.decode("latin1")
            self.lines.append(cmd)
            if cmd.startswith("echo '") and ">>" in cmd:
                self.echo_count += 1
                if self.echo_count == self.drop_line:
                    self.out += b"# "
                    continue
            if not cmd.strip():
                self.out += b"\n# "
                continue
            res = subprocess.run(["/bin/sh", "-c", self.map(cmd)],
                                 capture_output=True, env={"PATH": str(self.path_dir)}, timeout=30)
            text = (res.stdout + res.stderr).replace(str(self.root).encode(), b"")
            self.out += text + b"\n# "
        return len(data)

    def read(self, n: int) -> bytes:
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk

    def flushInput(self):  # pyserial name, used by older versions of the tool
        self.out = b""

    def close(self):
        pass


class BootstrapSerial(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.root = base / "remote"
        for d in ("data", "etc/init.d", "tmp", "usr/bin", "home/root", "root"):
            (self.root / d).mkdir(parents=True)
        self.rcs_on_device = self.root / "etc" / "init.d" / "rcS.local"
        self.rcs_on_device.write_text("#!/bin/sh\n# the boot script that was there before\n")
        self.path_dir = base / "pathbin"
        self.path_dir.mkdir()
        for t in SHELL_TOOLS:
            os.symlink(shutil.which(t), self.path_dir / t)
        self.host_decoder = base / "hostdec.py"
        self.host_decoder.write_text(HOST_DECODER.replace("/usr/bin/env python3", sys.executable))
        self.host_decoder.chmod(0o755)
        # Small stand-ins for the binaries (the real ones are ARM programs)
        self.dropbear = base / "dropbearmulti"
        self.dropbear.write_bytes(os.urandom(5000))
        self.bin_dir = base / "bin"
        self.bin_dir.mkdir()
        for b in ("dropbear", "dropbearkey", "scp"):
            shutil.copy(self.dropbear, self.bin_dir / b)
        self.pubkey = "ssh-ed25519 AAAAtest it's-a-comment\n"

    def tearDown(self):
        self.tmp.cleanup()

    def install_lua(self):
        # Decodes like the tool's Lua script, after checking the script arrived unchanged
        import setup_remote
        expected = pathlib.Path(self.tmp.name) / "decoder.lua"
        expected.write_text(setup_remote.LUA_DECODER_ONELINE)
        lua = self.root / "usr" / "bin" / "lua"
        lua.write_text(HOST_DECODER.replace("/usr/bin/env python3", sys.executable).replace(
            "import base64, sys\n",
            f"import base64, sys\nif open(sys.argv[1]).read() != open({str(expected)!r}).read(): sys.exit(3)\n"))
        lua.chmod(0o755)

    def load_tool(self, console: FakeConsole):
        fake_serial = types.ModuleType("serial")
        fake_serial.Serial = lambda *a, **k: console
        with mock.patch.dict(sys.modules, {"serial": fake_serial}):
            sys.modules.pop("bootstrap_serial", None)
            mod = importlib.import_module("bootstrap_serial")
        mod.BIN_DIR = self.bin_dir
        if hasattr(mod, "DROPBEAR_PATH"):
            mod.DROPBEAR_PATH = self.dropbear
        return mod

    def run_tool(self, drop_line: int | None = None, key_arg: bool = True,
                 home: pathlib.Path | None = None) -> tuple[int, str, FakeConsole]:
        console = FakeConsole(self.root, self.path_dir, self.host_decoder, drop_line)
        keyfile = pathlib.Path(self.tmp.name) / "key.pub"
        keyfile.write_text(self.pubkey)
        mod = self.load_tool(console)
        argv = ["bootstrap_serial.py", "--port", "fake"] + (["--ssh-pubkey", str(keyfile)] if key_arg else [])
        home = home or pathlib.Path(self.tmp.name) / "home"
        out = io.StringIO()
        code = 0
        with mock.patch.object(mod.time, "sleep", lambda s: None), \
             mock.patch.object(sys, "argv", argv), \
             mock.patch.dict(os.environ, {"HOME": str(home)}), \
             contextlib.redirect_stdout(out):
            try:
                mod.main()
            except SystemExit as e:
                code = int(e.code or 0)
        return code, out.getvalue(), console

    def test_without_lua_nothing_is_damaged(self):
        before = self.rcs_on_device.read_bytes()
        code, out, _ = self.run_tool()
        self.assertNotEqual(code, 0, out)
        self.assertNotIn("Bootstrap Complete", out)
        self.assertEqual(self.rcs_on_device.read_bytes(), before, "the boot script was changed")
        self.assertFalse((self.root / "data" / "dropbear").exists() and
                         (self.root / "data" / "dropbear").stat().st_size == 0, "an empty binary was installed")

    def test_with_lua_everything_arrives(self):
        self.install_lua()
        code, out, console = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertEqual((self.root / "data" / "dropbearmulti").read_bytes(), self.dropbear.read_bytes())
        rcs = (REPO_DIR / "remote" / "rcS.local").read_bytes().replace(b"\r\n", b"\n")
        self.assertEqual(self.rcs_on_device.read_bytes(), rcs)
        for p in ("etc/dropbear/authorized_keys", "home/root/.ssh/authorized_keys", "root/.ssh/authorized_keys"):
            self.assertEqual((self.root / p).read_text(), self.pubkey, p)
        start = [c for c in console.lines if c.startswith("/data/dropbear ")]
        self.assertTrue(start)
        self.assertNotIn("-B", start[-1].split(), "dropbear started with empty-password logins allowed")
        self.assertTrue(all(len(c) < 1024 for c in console.lines), "a command line longer than the shell takes")

    def test_lost_line_is_caught(self):
        self.install_lua()
        before = self.rcs_on_device.read_bytes()
        # The decoder file is sent first (a few lines), then the dropbear binary: lose one of its lines
        code, out, _ = self.run_tool(drop_line=12)
        self.assertNotEqual(code, 0, out)
        self.assertIn("did not arrive intact", out)
        self.assertEqual(self.rcs_on_device.read_bytes(), before)
        self.assertFalse((self.root / "data" / "dropbearmulti").exists())

    def test_owner_key_without_option(self):
        """No --ssh-pubkey: the newest ~/.ssh/harmony_owner_* key goes in, as setup_remote.py does."""
        self.install_lua()
        home = pathlib.Path(self.tmp.name) / "home"
        ssh = home / ".ssh"
        ssh.mkdir(parents=True)
        (ssh / "harmony_owner_ed25519").write_text("private\n")
        (ssh / "harmony_owner_ed25519.pub").write_text(self.pubkey)
        code, out, _ = self.run_tool(key_arg=False, home=home)
        self.assertEqual(code, 0, out)
        for p in ("etc/dropbear/authorized_keys", "home/root/.ssh/authorized_keys", "root/.ssh/authorized_keys"):
            self.assertTrue((self.root / p).is_file(), f"{p} missing: nobody could log in")
            self.assertEqual((self.root / p).read_text(), self.pubkey, p)

    def test_no_key_sends_nothing(self):
        console = FakeConsole(self.root, self.path_dir, self.host_decoder, None)
        mod = self.load_tool(console)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(mod.BootstrapError):
                mod.bootstrap(console, None)
            with self.assertRaises(mod.BootstrapError):
                mod.bootstrap(console, "  \n")
        self.assertEqual(console.lines, [], "commands were sent although no key can be installed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
