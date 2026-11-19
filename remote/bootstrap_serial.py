#!/usr/bin/env python3
"""Harmony Remote Serial Bootstrap Utility.

Bootstraps USB Ethernet, Dropbear SSH, and safe boot scripts on Logitech
the Harmony remote over UART (Serial console at 115200 8N1).

The stock firmware has no base64 or uudecode. As setup_remote.py does, the files go
over as base64 text, the small native decoder (b64dec_tiny) is decoded first with the
stock Lua, and every file is checked with md5sum before it gets its final name. A file
that does not arrive intact stops the tool, and the boot script already on the remote
is left as it is.

Usage:
    python remote/bootstrap_serial.py --port COM3 [--ssh-pubkey ~/.ssh/harmony_owner_ed25519.pub]

Without --ssh-pubkey the owner key setup_remote.py uses is installed (the newest
~/.ssh/harmony_owner_* key, created when there is none): the stock root account has a
password nobody knows, so Dropbear without a key would let nobody in.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import pathlib
import re
import sys
import time

try:
    import serial
except ImportError:
    serial = None  # checked in main(): the tests drive the functions with a fake port

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from setup_remote import DROPBEAR_PATH, LUA_DECODER_ONELINE, TINY_PATH, ensure_ssh_key  # noqa: E402

BIN_DIR = SCRIPT_DIR / "bin"
RCS_LOCAL_FILE = SCRIPT_DIR / "rcS.local"

# Characters per command line: BusyBox line editing takes 1024, keep well below
LINE_CHUNK = 384
DECODER = "/tmp/b64dec_tiny"


class BootstrapError(RuntimeError):
    pass


def read_until_prompt(ser, timeout: float = 5.0) -> str:
    """Read serial until shell prompt '#' or timeout."""
    ser.timeout = 0.2
    start = time.time()
    buf = ""
    while time.time() - start < timeout:
        raw = ser.read(1024)
        if raw:
            buf += raw.decode("latin1", errors="replace")
            if "#" in buf.strip()[-3:]:
                return buf
    return buf


def exec_cmd(ser, cmd: str, timeout: float = 8.0) -> str:
    """Execute command on remote shell and return output."""
    ser.write((cmd + "\n").encode("latin1"))
    time.sleep(0.1)
    return read_until_prompt(ser, timeout)


def sh_quote(text: str) -> str:
    return "'" + text.replace("'", "'\\''") + "'"


def remote_md5(ser, path: str) -> str | None:
    out = exec_cmd(ser, f"md5sum {path} 2>/dev/null")
    m = re.search(r"\b([0-9a-f]{32})\s+" + re.escape(path), out)
    return m.group(1) if m else None


def send_b64(ser, data: bytes, b64_path: str) -> None:
    """Append data as base64 text to b64_path on the remote, in short lines."""
    b64 = base64.b64encode(data).decode("ascii")
    exec_cmd(ser, f"rm -f {b64_path}")
    total = (len(b64) + LINE_CHUNK - 1) // LINE_CHUNK
    for n, i in enumerate(range(0, len(b64), LINE_CHUNK), 1):
        exec_cmd(ser, f"echo '{b64[i:i + LINE_CHUNK]}' >> {b64_path}", timeout=2.0)
        if n % 100 == 0 or n == total:
            print(f"    {n}/{total} lines")


def install_file(ser, data: bytes, remote_path: str, mode: str = "755") -> None:
    """Send data, decode it to <remote_path>.new, check its MD5, then move it into place.
    On a mismatch nothing is replaced and BootstrapError is raised."""
    tmp_b64 = "/tmp/bootstrap.b64"
    new = f"{remote_path}.new"
    print(f"[*] Uploading {remote_path} ({len(data)} bytes)...")
    send_b64(ser, data, tmp_b64)
    exec_cmd(ser, f"rm -f {new}; {DECODER} {tmp_b64} {new}; rm -f {tmp_b64}", timeout=30.0)
    want = hashlib.md5(data).hexdigest()
    got = remote_md5(ser, new)
    if got != want:
        exec_cmd(ser, f"rm -f {new}")
        raise BootstrapError(f"{remote_path} did not arrive intact (MD5 {got or 'missing'}, expected {want}); "
                             "nothing was replaced")
    exec_cmd(ser, f"chmod {mode} {new} && mv -f {new} {remote_path}")
    if remote_md5(ser, remote_path) != want:
        raise BootstrapError(f"moving {new} to {remote_path} failed")
    print(f"[+] {remote_path} verified ({want})")


def stage_decoder(ser) -> None:
    """Put the native base64 decoder in /tmp, decoded by the stock Lua (setup_remote.py's way)."""
    tiny = TINY_PATH.read_bytes()
    print(f"[*] Staging the native base64 decoder ({len(tiny)} bytes)...")
    exec_cmd(ser, "rm -f /tmp/bootstrap.lua")
    for i in range(0, len(LUA_DECODER_ONELINE), 200):
        exec_cmd(ser, f"printf '%s' {sh_quote(LUA_DECODER_ONELINE[i:i + 200])} >> /tmp/bootstrap.lua", timeout=2.0)
    send_b64(ser, tiny, "/tmp/tiny.b64")
    exec_cmd(ser, f"rm -f {DECODER}; /usr/bin/lua /tmp/bootstrap.lua /tmp/tiny.b64 {DECODER}; "
                  "rm -f /tmp/bootstrap.lua /tmp/tiny.b64", timeout=60.0)
    if remote_md5(ser, DECODER) != hashlib.md5(tiny).hexdigest():
        raise BootstrapError("the native decoder did not arrive intact (is /usr/bin/lua there?); nothing was installed")
    exec_cmd(ser, f"chmod 755 {DECODER}")
    print("[+] Native decoder verified.")


def bootstrap(ser, pubkey: str | None) -> None:
    # Dropbear only takes key logins (step 6) and the stock root password is unknown: without
    # a key the remote would end up with an SSH server nobody can log in to
    if not pubkey or not pubkey.strip():
        raise BootstrapError("no SSH public key to install; nothing was changed")

    # 1. Enable USB Ethernet
    print("[*] Starting USB Ethernet gadget...")
    exec_cmd(ser, "/usr/bin/usbeth start")
    exec_cmd(ser, "ifconfig usb0 192.168.2.1 up 2>/dev/null || true")
    exec_cmd(ser, "mkdir -p /data /etc/dropbear /home/root/.ssh /root/.ssh")

    stage_decoder(ser)

    # 2. Dropbear (one multi-call binary, as setup_remote.py installs it) and the sync helper
    install_file(ser, DROPBEAR_PATH.read_bytes(), "/data/dropbearmulti")
    exec_cmd(ser, "for n in dropbear dropbearkey scp; do ln -sf /data/dropbearmulti /data/$n; done")
    sync_bin = BIN_DIR / "remote_sync"
    if sync_bin.is_file():
        install_file(ser, sync_bin.read_bytes(), "/data/remote_sync")
        exec_cmd(ser, "ln -sf /data/remote_sync /data/sync; ln -sf /data/remote_sync /usr/bin/sync 2>/dev/null || true")

    # 3. Generate Dropbear host keys if missing
    print("[*] Checking Dropbear host keys...")
    exec_cmd(ser, "[ -f /etc/dropbear/dropbear_rsa_host_key ] || /data/dropbearkey -t rsa -f /etc/dropbear/dropbear_rsa_host_key", timeout=60.0)
    exec_cmd(ser, "[ -f /etc/dropbear/dropbear_ed25519_host_key ] || /data/dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key 2>/dev/null || true", timeout=60.0)

    # 4. Install the SSH public key where the other tools put it (sent as base64: a key
    #    comment may hold quotes)
    print("[*] Installing SSH public key...")
    install_file(ser, (pubkey.strip() + "\n").encode("utf-8"), "/etc/dropbear/authorized_keys", mode="600")
    exec_cmd(ser, "cp -f /etc/dropbear/authorized_keys /home/root/.ssh/authorized_keys; "
                  "cp -f /etc/dropbear/authorized_keys /root/.ssh/authorized_keys; "
                  "chmod 600 /home/root/.ssh/authorized_keys /root/.ssh/authorized_keys; "
                  "chmod 700 /home/root/.ssh /root/.ssh")

    # 5. The boot script: replaced only by a verified copy
    if RCS_LOCAL_FILE.is_file():
        print("[*] Installing /etc/init.d/rcS.local with recovery loop...")
        rcs = RCS_LOCAL_FILE.read_bytes().replace(b"\r\n", b"\n")
        install_file(ser, rcs, "/etc/init.d/rcS.local")

    exec_cmd(ser, f"rm -f {DECODER}")

    # 6. Start Dropbear now: key logins only (-B would allow logins with an empty password)
    print("[*] Starting Dropbear daemon...")
    exec_cmd(ser, "/data/dropbear -R")


def main() -> None:
    parser = argparse.ArgumentParser(description="Bootstrap Dropbear SSH on a Harmony remote via Serial")
    parser.add_argument("--port", required=True, help="Serial port (e.g. COM3 or /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200)")
    parser.add_argument("--ssh-pubkey", type=pathlib.Path,
                        help="Path to SSH public key to authorize (default: the owner key setup_remote.py uses, "
                             "~/.ssh/harmony_owner_*.pub, created when missing)")
    args = parser.parse_args()

    if serial is None:
        print("[-] pyserial not found. Install with: pip install pyserial")
        sys.exit(1)
    for f in (TINY_PATH, DROPBEAR_PATH):
        if not f.is_file():
            print(f"[-] Missing {f}")
            sys.exit(1)
    pub_path = args.ssh_pubkey or ensure_ssh_key()
    if not pub_path.is_file():
        print(f"[-] Public key not found: {pub_path}")
        sys.exit(1)
    pubkey = pub_path.read_text()
    if not pubkey.strip():
        print(f"[-] Public key file is empty: {pub_path}")
        sys.exit(1)
    print(f"[*] Authorizing the key in {pub_path}")

    print("=" * 60)
    print("  Harmony Remote Serial Bootstrap Utility")
    print(f"  Port: {args.port} @ {args.baud} 8N1")
    print("=" * 60)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1.0)
    except Exception as e:
        print(f"[-] Failed to open serial port: {e}")
        sys.exit(1)

    try:
        # Probe shell
        ser.write(b"\n\n")
        time.sleep(0.5)
        out = read_until_prompt(ser, timeout=3.0)
        if "#" not in out:
            print("[*] Sending newline to wake shell...")
            ser.write(b"\n")
            time.sleep(0.5)
            out = read_until_prompt(ser, timeout=3.0)
        if "#" not in out:
            print("[-] No root shell prompt (#) on the serial console. Log in on the console first, then retry.")
            sys.exit(1)
        print("[+] Shell connected.")

        try:
            bootstrap(ser, pubkey)
        except BootstrapError as e:
            print(f"[-] {e}")
            print("[-] Serial bootstrap NOT complete.")
            sys.exit(1)

        print()
        print("=" * 60)
        print("  [+] Remote Serial Bootstrap Complete!")
        print("  Connect PC USB to the remote cradle/port.")
        print("  Configure PC adapter: IP 192.168.2.2 / Subnet 255.255.255.0")
        print("  Connect via SSH:")
        print("      ssh root@192.168.2.1")
        print("=" * 60)

    finally:
        ser.close()


if __name__ == "__main__":
    if sys.version_info < (3, 13):
        sys.exit(f"This tool needs Python 3.13 or newer (this is Python {sys.version.split()[0]}).")
    main()
