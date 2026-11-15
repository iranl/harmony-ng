#!/usr/bin/env python3
"""
Malformed input to hub_daemon's network parsers, on hub_daemon_test: the HTTP request parser of
the API port and of the Logitech compatibility port, and the SSDP M-SEARCH parser. After each
bad request the daemon must still be running and answer a good request; a malformed M-SEARCH
gets no reply. The existing tests send well-formed requests only (plus one unknown search
target).
"""

import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 9268
LEGACY_PORT = 9269
SSDP_PORT = 9270
URN = "urn:myharmony-com:device:harmony:1"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def raw(port, data, read=True, timeout=3.0):
    """Sends bytes, half-closes, returns what came back (b"" on reset or timeout)"""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    except OSError:
        return None
    out = b""
    try:
        s.sendall(data)
        s.shutdown(socket.SHUT_WR)
        while read:
            c = s.recv(65536)
            if not c:
                break
            out += c
    except (OSError, socket.timeout):
        pass
    s.close()
    return out


def good_request(port, path):
    out = raw(port, ("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n" % path).encode())
    return out is not None and out.startswith(b"HTTP/1.1 200")


HTTP_CASES = [
    ("empty connection", b""),
    ("binary junk", bytes(range(256)) * 4),
    ("no request line ending", b"GET /health HTTP/1.1"),
    ("method only", b"GET\r\n\r\n"),
    ("request line without a version", b"GET /health\r\n\r\n"),
    ("path without a slash", b"GET health HTTP/1.1\r\n\r\n"),
    ("header without a colon", b"GET /health HTTP/1.1\r\nHost 127.0.0.1\r\n\r\n"),
    ("header line of 16 KiB", b"GET /health HTTP/1.1\r\nX-Long: " + b"a" * 16384 + b"\r\n\r\n"),
    ("request line of 16 KiB", b"GET /" + b"a" * 16384 + b" HTTP/1.1\r\n\r\n"),
    ("negative Content-Length", b"POST /api/settings HTTP/1.1\r\nContent-Length: -5\r\n\r\n{}"),
    ("huge Content-Length", b"POST /api/settings HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n{}"),
    ("Content-Length larger than the body", b"POST /api/settings HTTP/1.1\r\nContent-Length: 4096\r\n\r\n{\"hub_name\":"),
    ("body that is not JSON", b"POST /api/settings HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 9\r\n\r\n{not json"),
    ("JSON nested 5000 deep", b"POST /api/settings HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 10000\r\n\r\n"
     + b"[" * 5000 + b"]" * 5000),
    ("NUL bytes in the path", b"GET /api/\x00status HTTP/1.1\r\n\r\n"),
    ("percent escapes cut short", b"GET /api/logs?source=%2 HTTP/1.1\r\n\r\n"),
    ("WebSocket upgrade without a key", b"GET /ws HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n"),
    ("bare LF line ends", b"GET /health HTTP/1.1\nHost: x\n\n"),
]

SSDP_CASES = [
    ("empty datagram", b""),
    ("binary junk", bytes(range(256))),
    ("M-SEARCH without ST", b"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n\r\n"),
    ("ST of 4000 bytes", b"M-SEARCH * HTTP/1.1\r\nMAN: \"ssdp:discover\"\r\nST: " + b"x" * 4000 + b"\r\n\r\n"),
    ("no line ends", b"M-SEARCH * HTTP/1.1 MAN: \"ssdp:discover\" ST: " + URN.encode()),
    ("header without a colon", b"M-SEARCH * HTTP/1.1\r\nMAN \"ssdp:discover\"\r\nST " + URN.encode() + b"\r\n\r\n"),
]


def settings_legacy_on():
    with open(os.path.join(SAMPLE_CONFIG, "settings.json")) as f:
        s = json.load(f)
    s["legacy_enabled"] = True
    return s


def msearch(data, timeout=1.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.bind(("127.0.0.1", 0))
    s.sendto(data, ("127.0.0.1", SSDP_PORT))
    try:
        reply, _ = s.recvfrom(4096)
    except socket.timeout:
        reply = b""
    s.close()
    return reply


def main():
    args = ["--legacy-port", str(LEGACY_PORT), "--ssdp-port", str(SSDP_PORT)]
    with HubHarness(port=PORT, args=args, extra_config_files={"settings.json": settings_legacy_on()}) as hub:
        time.sleep(0.5)
        for label, port, path in (("API port", PORT, "/health"), ("compatibility port", LEGACY_PORT, "/description")):
            print("[-] HTTP parser of the %s" % label)
            check(good_request(port, path), "good request answered before")
            for name, data in HTTP_CASES:
                raw(port, data)
                alive = hub.proc.poll() is None
                check(alive and good_request(port, path), "%s: daemon alive, next request answered" % name)
                if not alive:
                    break

        print("[-] SSDP M-SEARCH parser")
        good = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n"
                % URN).encode()
        check(msearch(good, 2.0).startswith(b"HTTP/1.1 200 OK"), "good M-SEARCH answered before")
        for name, data in SSDP_CASES:
            reply = msearch(data)
            check(reply == b"", "%s: no reply" % name, reply[:60])
        check(hub.proc.poll() is None and msearch(good, 2.0).startswith(b"HTTP/1.1 200 OK"),
              "good M-SEARCH answered after")

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] malformed HTTP and SSDP input: all checks passed")


if __name__ == "__main__":
    main()
