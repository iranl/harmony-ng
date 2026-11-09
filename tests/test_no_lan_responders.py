#!/usr/bin/env python3
"""
The test build of hub_daemon must not announce itself on a real network (a test run used to show
up in Home Assistant as a discovered "Test Hub"). mDNS: without --mdns-port, or with 5353, the
responder of hub_daemon_test stays off and binds nothing on 5353; with a test port its probes and
announcements reach a listener joined on loopback and never one joined on the LAN interface.
SSDP (legacy_enabled on): the same, for port 1900 and the group 239.255.255.250.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402
from lan_guard import udp_ports_of, lan_address, group_listener, collect, send_local_multicast  # noqa: E402

PORT = 8984
MDNS_GROUP = "224.0.0.251"
TEST_MDNS_PORT = 5356
SSDP_GROUP = "239.255.255.250"
TEST_SSDP_PORT = 11901
LEGACY_PORT = 18089
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def wait_log(path, needle, seconds=5.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            with open(path, errors="replace") as f:
                if needle in f.read():
                    return True
        except OSError:
            pass
        time.sleep(0.1)
    return False


def is_dns_response(data):
    return len(data) >= 12 and (data[2] & 0x80) != 0


def legacy_settings():
    import json
    sample = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "ha", "sample_config", "settings.json")
    with open(sample) as f:
        st = json.load(f)
    st["legacy_enabled"] = True
    return st


def main():
    import tempfile
    log = tempfile.NamedTemporaryFile(prefix="nolan_", suffix=".log", delete=False).name
    try:
        print("[-] default arguments: mDNS off, nothing bound on 5353")
        with HubHarness(port=PORT, log_path=log) as hub:
            check(wait_log(log, "mDNS: off in the test build"), "daemon says mDNS is off")
            ports = udp_ports_of(hub.proc.pid)
            check(5353 not in ports, "no socket on 5353", str(sorted(ports)))
            st, s = hub.http_get("/api/status")
            check(st == 200 and s.get("mdns_enabled") is True, "the setting keeps the product default (true)", str(s.get("mdns_enabled")))

        print("[-] --mdns-port 5353 is refused in the test build")
        with HubHarness(port=PORT, args=["--mdns-port", "5353"], log_path=log) as hub:
            check(wait_log(log, "mDNS: off in the test build"), "daemon says mDNS is off")
            check(5353 not in udp_ports_of(hub.proc.pid), "no socket on 5353")

        print("[-] --mdns-port %d: announcements on loopback only" % TEST_MDNS_PORT)
        lan = lan_address()
        lo_l = group_listener(MDNS_GROUP, TEST_MDNS_PORT, "127.0.0.1")
        lan_l = group_listener(MDNS_GROUP, TEST_MDNS_PORT, lan) if lan else None
        if lan_l:
            # the listeners tell the interfaces apart: a control packet via the LAN interface
            # reaches the LAN listener only (TTL 0: it never leaves the host)
            send_local_multicast(MDNS_GROUP, TEST_MDNS_PORT, lan)
            got_lan = collect(lan_l, 0.5)
            got_lo = collect(lo_l, 0.3)
            check(len(got_lan) == 1 and got_lo == [], "control: LAN and loopback listeners are told apart",
                  "lan=%d lo=%d" % (len(got_lan), len(got_lo)))
        with HubHarness(port=PORT, args=["--mdns-port", str(TEST_MDNS_PORT)], log_path=log) as hub:
            seen_lo = collect(lo_l, 4.0, is_dns_response)   # probes take 750 ms, two announcements 1 s apart
            check(len(seen_lo) >= 2, "announcements received on loopback (%d)" % len(seen_lo))
            check(TEST_MDNS_PORT in udp_ports_of(hub.proc.pid) and 5353 not in udp_ports_of(hub.proc.pid),
                  "responder on the test port only")
            if lan_l:
                seen_lan = collect(lan_l, 0.5)
                check(seen_lan == [], "nothing received on the LAN interface %s" % lan, str(seen_lan[:1]))
            else:
                print("  (no LAN interface on this host: LAN check skipped)")
        lo_l.close()
        if lan_l:
            lan_l.close()

        print("[-] legacy_enabled on, default arguments: SSDP off, nothing bound on 1900")
        cfg = {"settings.json": legacy_settings()}
        with HubHarness(port=PORT, args=["--legacy-port", str(LEGACY_PORT)], extra_config_files=cfg, log_path=log) as hub:
            check(wait_log(log, "SSDP: off in the test build"), "daemon says SSDP is off")
            check(1900 not in udp_ports_of(hub.proc.pid), "no socket on 1900", str(sorted(udp_ports_of(hub.proc.pid))))
            st, s = hub.http_get("/api/status")
            check(st == 200 and s.get("legacy_enabled") is True, "legacy_enabled is on (the shim itself runs)")

        print("[-] --ssdp-port 1900 is refused in the test build")
        with HubHarness(port=PORT, args=["--legacy-port", str(LEGACY_PORT), "--ssdp-port", "1900"],
                        extra_config_files=cfg, log_path=log) as hub:
            check(wait_log(log, "SSDP: off in the test build"), "daemon says SSDP is off")
            check(1900 not in udp_ports_of(hub.proc.pid), "no socket on 1900")

        print("[-] --ssdp-port %d: NOTIFY on loopback only" % TEST_SSDP_PORT)
        lo_l = group_listener(SSDP_GROUP, TEST_SSDP_PORT, "127.0.0.1")
        lan_l = group_listener(SSDP_GROUP, TEST_SSDP_PORT, lan) if lan else None
        if lan_l:
            send_local_multicast(SSDP_GROUP, TEST_SSDP_PORT, lan)
            got_lan = collect(lan_l, 0.5)
            got_lo = collect(lo_l, 0.3)
            check(len(got_lan) == 1 and got_lo == [], "control: LAN and loopback listeners are told apart",
                  "lan=%d lo=%d" % (len(got_lan), len(got_lo)))
        with HubHarness(port=PORT, args=["--legacy-port", str(LEGACY_PORT), "--ssdp-port", str(TEST_SSDP_PORT)],
                        extra_config_files=cfg, log_path=log) as hub:
            seen_lo = collect(lo_l, 2.0, lambda d: d.startswith(b"NOTIFY"))
            check(len(seen_lo) >= 1 and b"ssdp:alive" in seen_lo[0][0], "NOTIFY alive received on loopback (%d)" % len(seen_lo))
            ports = udp_ports_of(hub.proc.pid)
            check(TEST_SSDP_PORT in ports and 1900 not in ports, "responder on the test port only", str(sorted(ports)))
            if lan_l:
                seen_lan = collect(lan_l, 0.5)
                check(seen_lan == [], "nothing received on the LAN interface %s" % lan, str(seen_lan[:1]))
        lo_l.close()
        if lan_l:
            lan_l.close()
    finally:
        try:
            os.remove(log)
        except OSError:
            pass
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all no-LAN checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
