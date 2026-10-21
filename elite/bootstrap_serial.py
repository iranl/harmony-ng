#!/usr/bin/env python3
"""Harmony Elite Serial Bootstrap Utility.

Bootstraps USB Ethernet, Dropbear SSH, and safe boot scripts on Logitech
Harmony Elite over UART (Serial console at 115200 8N1).

Usage:
    python bootstrap_serial.py --port COM3 [--ssh-pubkey ~/.ssh/id_rsa.pub]
"""

from __future__ import annotations

import argparse
import base64
import pathlib
import sys
import time

try:
    import serial
except ImportError:
    print("[-] pyserial not found. Install with: pip install pyserial")
    sys.exit(1)

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
BIN_DIR = SCRIPT_DIR / "bin"
RCS_LOCAL_FILE = SCRIPT_DIR / "rcS.local"


def read_until_prompt(ser: serial.Serial, timeout: float = 5.0) -> str:
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


def exec_cmd(ser: serial.Serial, cmd: str, timeout: float = 8.0) -> str:
    """Execute command on remote shell and return output."""
    ser.write((cmd + "\n").encode("latin1"))
    time.sleep(0.1)
    return read_until_prompt(ser, timeout)


def transfer_binary(ser: serial.Serial, local_file: pathlib.Path, remote_path: str) -> bool:
    """Transfer file over serial using base64 chunks."""
    if not local_file.is_file():
        print(f"[-] Missing binary: {local_file}")
        return False

    data = local_file.read_bytes()
    b64 = base64.b64encode(data).decode("ascii")
    print(f"[*] Uploading {local_file.name} ({len(data)} bytes, {len(b64)} b64)...")

    # Clear temp target
    exec_cmd(ser, f"rm -f /tmp/b64_transfer {remote_path}")

    # Chunk base64 lines (512 chars each)
    chunk_size = 512
    total_chunks = (len(b64) + chunk_size - 1) // chunk_size
    for i in range(0, len(b64), chunk_size):
        chunk = b64[i : i + chunk_size]
        ser.write(f"echo '{chunk}' >> /tmp/b64_transfer\n".encode("latin1"))
        time.sleep(0.04)
        if (i // chunk_size) % 20 == 0:
            print(f"    Progress: {(i // chunk_size) + 1}/{total_chunks} chunks")

    time.sleep(0.5)
    ser.flushInput()

    # Decode and check size
    exec_cmd(ser, f"base64 -d /tmp/b64_transfer > {remote_path} || uudecode -o {remote_path} /tmp/b64_transfer")
    exec_cmd(ser, f"chmod 755 {remote_path}; rm -f /tmp/b64_transfer")
    out = exec_cmd(ser, f"ls -l {remote_path}")
    print(f"    Target: {out.strip()}")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Bootstrap Dropbear SSH on Harmony Elite via Serial")
    parser.add_argument("--port", required=True, help="Serial port (e.g. COM3 or /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200)")
    parser.add_argument("--ssh-pubkey", type=pathlib.Path, help="Path to SSH public key to authorize")
    args = parser.parse_args()

    print("=" * 60)
    print("  Harmony Elite Serial Bootstrap Utility")
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

        print("[+] Shell connected.")

        # 1. Enable USB Ethernet
        print("[*] Starting USB Ethernet gadget...")
        exec_cmd(ser, "/usr/bin/usbeth start")
        exec_cmd(ser, "ifconfig usb0 192.168.2.1 up 2>/dev/null || true")

        # 2. Stage binaries into /data/
        exec_cmd(ser, "mkdir -p /data /etc/dropbear /home/root/.ssh")

        for b in ("dropbear", "dropbearkey", "scp", "codex_sync"):
            src = BIN_DIR / b
            if src.is_file():
                transfer_binary(ser, src, f"/data/{b}")

        # Symlink sync
        exec_cmd(ser, "ln -sf /data/codex_sync /data/sync; ln -sf /data/codex_sync /usr/bin/sync 2>/dev/null || true")

        # 3. Generate Dropbear host keys if missing
        print("[*] Checking Dropbear host keys...")
        exec_cmd(ser, "if [ ! -f /etc/dropbear/dropbear_rsa_host_key ]; then /data/dropbearkey -t rsa -f /etc/dropbear/dropbear_rsa_host_key; fi")
        exec_cmd(ser, "if [ ! -f /etc/dropbear/dropbear_ed25519_host_key ]; then /data/dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key 2>/dev/null || true; fi")

        # 4. Install SSH public key
        if args.ssh_pubkey and args.ssh_pubkey.is_file():
            key_content = args.ssh_pubkey.read_text().strip()
            print("[*] Installing SSH public key...")
            exec_cmd(ser, f"echo '{key_content}' > /home/root/.ssh/authorized_keys")
            exec_cmd(ser, "chmod 600 /home/root/.ssh/authorized_keys")

        # 5. Install /etc/init.d/rcS.local
        if RCS_LOCAL_FILE.is_file():
            print("[*] Installing /etc/init.d/rcS.local with recovery loop...")
            rcs_text = RCS_LOCAL_FILE.read_text().replace("\r\n", "\n")
            b64_rcs = base64.b64encode(rcs_text.encode("utf-8")).decode("ascii")
            exec_cmd(ser, f"echo '{b64_rcs}' | base64 -d > /etc/init.d/rcS.local")
            exec_cmd(ser, "chmod 755 /etc/init.d/rcS.local")

        # 6. Start Dropbear now
        print("[*] Starting Dropbear daemon...")
        exec_cmd(ser, "/data/dropbear -R -B")

        print()
        print("=" * 60)
        print("  [+] Elite Serial Bootstrap Complete!")
        print("  Connect PC USB to Elite remote cradle/port.")
        print("  Configure PC adapter: IP 192.168.2.2 / Subnet 255.255.255.0")
        print("  Connect via SSH:")
        print("      ssh root@192.168.2.1")
        print("=" * 60)

    finally:
        ser.close()


if __name__ == "__main__":
    main()
