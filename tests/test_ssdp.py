#!/usr/bin/env python3
"""
SSDP responder (spec 6.6) against hub_daemon_test with legacy_enabled and --ssdp-port: M-SEARCH
over loopback for the three search targets, the reply headers, the UUID, /description on the
legacy port, NOTIFY alive on enable and byebye on disable, and the setting toggling the socket.
"""

import json
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8994
LEGACY_PORT = 18089
SSDP_PORT = 11900
URN = "urn:myharmony-com:device:harmony:1"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def settings(enabled):
    with open(os.path.join(SAMPLE_CONFIG, "settings.json")) as f:
        s = json.load(f)
    s["legacy_enabled"] = enabled
    return s


def headers(text):
    lines = text.split("\r\n")
    out = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            out[k.strip().upper()] = v.strip()
    return lines[0], out


def msearch(st, timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.bind(("127.0.0.1", 0))
    req = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n" % st)
    s.sendto(req.encode(), ("127.0.0.1", SSDP_PORT))
    try:
        data, _ = s.recvfrom(4096)
    except socket.timeout:
        data = b""
    s.close()
    return data.decode(errors="replace")


def listen_multicast(timeout):
    """Collects NOTIFY packets on the test port for timeout seconds"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.settimeout(timeout)
    s.bind(("", SSDP_PORT))
    for iface in (socket.inet_aton("0.0.0.0"), socket.inet_aton("127.0.0.1")):
        try:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton("239.255.255.250") + iface)
        except OSError:
            pass
    return s


def collect(s, timeout):
    out = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s.settimeout(max(0.05, deadline - time.monotonic()))
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            break
        out.append(data.decode(errors="replace"))
    return out


def http_get(path):
    conn = socket.create_connection(("127.0.0.1", LEGACY_PORT), timeout=3.0)
    conn.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n" % path).encode())
    data = b""
    while True:
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    conn.close()
    head, _, payload = data.partition(b"\r\n\r\n")
    return int(head.split(b" ")[1]), payload.decode()


def main():
    args = ["--legacy-port", str(LEGACY_PORT), "--ssdp-port", str(SSDP_PORT)]
    print("[-] 6.6 M-SEARCH on port %d" % SSDP_PORT)
    with HubHarness(port=PORT, args=args, extra_config_files={"settings.json": settings(True)}) as hub:
        time.sleep(0.5)
        reply = msearch(URN)
        check(reply.startswith("HTTP/1.1 200 OK"), "reply to the harmony URN", reply[:60])
        status, h = headers(reply)
        check(h.get("CACHE-CONTROL") == "max-age=1800" and "EXT" in h, "CACHE-CONTROL and EXT", str(h))
        loc = h.get("LOCATION", "")
        check(re.match(r"^http://[0-9.]+:%d/description$" % LEGACY_PORT, loc) is not None, "LOCATION on the legacy port", loc)
        check(h.get("SERVER", "").startswith("Linux/2.6 UPnP/1.0 HarmonyNG/"), "SERVER header", h.get("SERVER"))
        check(h.get("ST") == URN, "ST echoed", h.get("ST"))
        usn = h.get("USN", "")
        m = re.match(r"^uuid:([0-9a-f]{8}-[0-9a-f]{4}-5[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})::" + re.escape(URN) + "$", usn)
        check(m is not None, "USN uuid v5 :: harmony URN", usn)
        uuid_v5 = m.group(1) if m else ""
        r2 = msearch("ssdp:all")
        _, h2 = headers(r2)
        check(h2.get("ST") == URN and h2.get("USN") == usn, "ssdp:all answered with the harmony URN", str(h2.get("ST")))
        r3 = msearch("upnp:rootdevice")
        _, h3 = headers(r3)
        check(h3.get("ST") == "upnp:rootdevice" and h3.get("USN") == "uuid:%s::upnp:rootdevice" % uuid_v5,
              "upnp:rootdevice answered, USN trails upnp:rootdevice (UPnP 1.0)", str(h3.get("USN")))
        r4 = msearch("urn:schemas-upnp-org:device:MediaRenderer:1")
        check(r4 == "", "other search targets get no reply", r4[:40])
        # same uuid on a restart: derived from hub_id
        st, xml = http_get("/description")
        check(st == 200 and ("<UDN>uuid:%s</UDN>" % uuid_v5) in xml, "/description UDN is the SSDP uuid", xml[:120])
        check("<deviceType>%s</deviceType>" % URN in xml and "<manufacturer>Logitech</manufacturer>" in xml
              and "<friendlyName>Test Hub</friendlyName>" in xml, "description deviceType, manufacturer, friendlyName")
        st2, status = hub.http_get("/api/status")
        check("<serialNumber>%s</serialNumber>" % status.get("hub_id") in xml, "serialNumber = hub_id")

        print("[-] NOTIFY byebye on disable, alive on enable")
        lsock = listen_multicast(3.0)
        hub.http_post("/api/settings", {"legacy_enabled": False})
        pk = collect(lsock, 1.5)
        bye = [p for p in pk if "NTS: ssdp:byebye" in p]
        check(len(bye) >= 1 and ("USN: " + usn) in bye[0], "byebye sent when the setting goes off", str(pk)[:300])
        check(msearch(URN, 1.0) == "", "no reply while off")
        hub.http_post("/api/settings", {"legacy_enabled": True})
        pk = collect(lsock, 1.5)
        alive = [p for p in pk if "NTS: ssdp:alive" in p]
        check(len(alive) >= 1 and "NT: %s" % URN in alive[0] and ("LOCATION: http://" in alive[0]), "alive sent when the setting goes on", str(pk)[:300])
        lsock.close()
        check(msearch(URN).startswith("HTTP/1.1 200 OK"), "answers again")

    print("[-] legacy_enabled off at start: no SSDP socket")
    with HubHarness(port=PORT, args=args, extra_config_files={"settings.json": settings(False)}) as hub:
        time.sleep(0.5)
        check(msearch(URN, 1.0) == "", "no reply with the setting off")

    print("\n%s" % ("ALL SSDP CHECKS PASSED" if failed == 0 else "%d SSDP CHECK(S) FAILED" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
