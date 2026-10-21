#!/usr/bin/env python3
"""
Simple TCP server to stream dropbearmulti to the Harmony Hub over LAN/Wi-Fi.
Run this on your PC while the Hub runs:
    /tmp/recv <PC_IP> 9999 > /usr/sbin/dropbearmulti
"""

import os
import socket
import sys

PORT = 9999
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DROPBEAR_PATH = os.path.join(SCRIPT_DIR, "..", "bin", "dropbearmulti_mips")

if not os.path.isfile(DROPBEAR_PATH):
    # Fallback to local or payload/bin/dropbearmulti
    alt1 = os.path.join(SCRIPT_DIR, "dropbearmulti")
    alt2 = os.path.join(SCRIPT_DIR, "..", "..", "payload", "bin", "dropbearmulti")
    if os.path.isfile(alt1):
        DROPBEAR_PATH = alt1
    elif os.path.isfile(alt2):
        DROPBEAR_PATH = alt2
    else:
        print(f"Error: dropbearmulti not found at {DROPBEAR_PATH}")
        sys.exit(1)

def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()

def main():
    local_ip = get_local_ip()
    size = os.path.getsize(DROPBEAR_PATH)
    print("=" * 60)
    print(f" Harmony Hub Offline Recovery TCP Server")
    print(f" File: {DROPBEAR_PATH} ({size:,} bytes)")
    print(f" Serving on {local_ip}:{PORT}")
    print("=" * 60)
    print(f"\nOn the Harmony Hub serial console, run:")
    print(f"  /tmp/recv {local_ip} {PORT} > /usr/sbin/dropbearmulti\n")

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", PORT))
    server.listen(1)

    print("Waiting for Hub to connect...")
    conn, addr = server.accept()
    print(f"Connected from {addr[0]}:{addr[1]}! Streaming binary...")

    with open(DROPBEAR_PATH, "rb") as f:
        data = f.read()
        conn.sendall(data)

    conn.close()
    server.close()
    print("Transfer completed successfully! The Hub now has dropbearmulti.")

if __name__ == "__main__":
    main()
