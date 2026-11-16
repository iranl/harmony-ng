#!/usr/bin/env python3
"""Network settings the Hub cannot use are refused before they are saved.

- POST /api/wifi-save and /api/wifi-test: a pass phrase outside 8 to 63 characters, or an SSID
  with a line break, gets 400 with the reason (wpa_supplicant would refuse the network block,
  or the line break would add lines to its file);
- POST /api/ethernet-save in static mode: an address outside the gateway's subnet, a netmask
  that is not a mask, a network or broadcast address, or a bad DNS entry gets 400 and
  ethernet.conf stays as it was; a working static setting and the dhcp mode are saved as before.

The daemon runs on a private mock root with --dry-run; nothing on the host is written (the
refused requests stop before any write).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_mock_lib  # noqa: E402

PORT = 9143


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + str(detail)))
        if not cond:
            failures.append(name)

    build_mock_lib()
    with HubHarness(port=PORT) as hub:
        conf = os.path.join(hub.data_dir, "ethernet.conf")

        for path in ("/api/wifi-save", "/api/wifi-test"):
            st, r = hub.http_post(path, {"ssid": "Home", "password": "short"})
            check(st == 400 and "8 to 63" in str(r), path + ": pass phrase of 5 characters refused", (st, r))
            st, r = hub.http_post(path, {"ssid": "Home\nnetwork={", "password": "password123"})
            check(st == 400 and "line break" in str(r), path + ": SSID with a line break refused", (st, r))

        good = {"eth_mode": "static", "eth_ip": "192.168.1.50", "eth_netmask": "255.255.255.0",
                "eth_gateway": "192.168.1.1", "eth_dns": "192.168.1.1, 9.9.9.9"}
        st, r = hub.http_post("/api/ethernet-save", good)
        check(st == 200, "working static setting saved", (st, r))
        saved = open(conf).read() if os.path.exists(conf) else ""
        check('ETH_IP="192.168.1.50"' in saved, "ethernet.conf holds the static address", saved)

        bad_cases = [
            ("address outside the gateway's subnet", {"eth_ip": "192.168.10.50"}),
            ("netmask that is not a mask", {"eth_netmask": "255.0.255.0"}),
            ("broadcast address", {"eth_ip": "192.168.1.255"}),
            ("network address", {"eth_ip": "192.168.1.0"}),
            ("invalid address", {"eth_ip": "192.168.1.500"}),
            ("bad DNS entry", {"eth_dns": "192.168.1.1 dns.example"}),
        ]
        for name, change in bad_cases:
            body = dict(good, **change)
            st, r = hub.http_post("/api/ethernet-save", body)
            check(st == 400, "static " + name + " refused", (st, r))
        after = open(conf).read() if os.path.exists(conf) else ""
        check(after == saved, "ethernet.conf unchanged by the refused requests", after)

        st, r = hub.http_post("/api/ethernet-save", {"eth_mode": "dhcp", "eth_ip": "not-an-ip"})
        check(st == 200 and 'ETH_MODE="dhcp"' in open(conf).read(), "dhcp mode saved as before", (st, r))

    if failures:
        print("network settings API: %d FAILED" % len(failures))
        return 1
    print("all network settings API checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
