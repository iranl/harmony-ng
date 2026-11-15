#!/usr/bin/env python3
"""
Simple TCP server to stream dropbearmulti to the Harmony Hub over LAN/Wi-Fi.
Run this on your PC while the Hub runs:
    /tmp/recv <PC_IP> 9999 > /tmp/dropbearmulti

Usage:
    python tools/unbrick_recovery/serve_dropbear.py --hub <HUB_IP> [--bind <PC_IP>] [--port 9999]

With --hub, only the Hub's address gets the file; any other host that connects first is
turned away and the server keeps waiting. The MD5 printed at the start is the one to
compare with `md5sum /tmp/dropbearmulti` on the Hub before the file is moved into place.
"""

import argparse
import hashlib
import os
import socket
import sys

PORT = 9999
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def find_dropbear():
    for cand in (os.path.join(SCRIPT_DIR, "..", "bin", "dropbearmulti_mips"),
                 os.path.join(SCRIPT_DIR, "dropbearmulti"),
                 os.path.join(SCRIPT_DIR, "..", "..", "payload", "bin", "dropbearmulti")):
        if os.path.isfile(cand):
            return os.path.normpath(cand)
    return None


def get_local_ip():
    """The address of the default route (a guess: with a VPN or several networks it may not be
    the one the Hub can reach; pass --bind then)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def serve(data, bind, port, hub=None, log=print, ready=None):
    """Send data to one client and return its address. With hub set, connections from any
    other address are closed without data and the server keeps waiting."""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind((bind, port))
        server.listen(4)
        if ready:
            ready()
        log("Waiting for Hub to connect...")
        while True:
            conn, addr = server.accept()
            with conn:
                if hub and addr[0] != hub:
                    log(f"Refused a connection from {addr[0]} (not the Hub at {hub}); still waiting.")
                    continue
                log(f"Connected from {addr[0]}:{addr[1]}! Streaming {len(data):,} bytes...")
                conn.sendall(data)
                return addr[0]
    finally:
        server.close()


def main():
    parser = argparse.ArgumentParser(description="Serve dropbearmulti to a Harmony Hub in serial recovery")
    parser.add_argument("--hub", help="The Hub's IP address: only it gets the file (recommended)")
    parser.add_argument("--bind", help="Address of this PC to listen on (default: the address of the default route)")
    parser.add_argument("--port", type=int, default=PORT, help=f"TCP port (default: {PORT})")
    args = parser.parse_args()

    path = find_dropbear()
    if not path:
        print("Error: dropbearmulti not found (tools/bin/dropbearmulti_mips)")
        sys.exit(1)
    with open(path, "rb") as f:
        data = f.read()
    md5 = hashlib.md5(data).hexdigest()
    bind = args.bind or get_local_ip()

    print("=" * 60)
    print(" Harmony Hub Offline Recovery TCP Server")
    print(f" File: {path} ({len(data):,} bytes)")
    print(f" MD5:  {md5}")
    print(f" Serving on {bind}:{args.port}" + (f", only to {args.hub}" if args.hub else ""))
    print("=" * 60)
    if not args.hub:
        print("[!] No --hub given: the first host that connects gets the file.")
    print("\nOn the Harmony Hub serial console, run:")
    print(f"  /tmp/recv {bind} {args.port} > /tmp/dropbearmulti")
    print("  md5sum /tmp/dropbearmulti")
    print(f"and move it into place only if the sum is {md5}:")
    print("  mv /tmp/dropbearmulti /usr/sbin/dropbearmulti\n")

    peer = serve(data, bind, args.port, args.hub)
    print(f"Sent {len(data):,} bytes to {peer}. Check the MD5 on the Hub before moving the file.")


if __name__ == "__main__":
    main()
