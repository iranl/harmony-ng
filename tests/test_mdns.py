#!/usr/bin/env python3
"""
mDNS responder (spec 4.1, 4.2) against hub_daemon_test started with --mdns-port 5354 (the test
build answers and announces over loopback only, see test_no_lan_responders.py): PTR query
over loopback gives PTR + SRV + TXT + A with the documented TXT keys, service enumeration, SRV/TXT
and A queries, known-answer suppression, a hub_name change renames the instance, and
mdns_enabled false closes the port (true reopens it).
"""

import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402
from mdns_client import query, encode_name, TYPE_A, TYPE_PTR, TYPE_TXT, TYPE_SRV, TYPE_ANY  # noqa: E402

PORT = 8990
MDNS_PORT = 5354
UNIT_ID = "20140101120000-000001"
SERVICE = "_harmony-ng._tcp.local"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def rec(s):
    b = s.encode("ascii") if isinstance(s, str) else s
    return struct.pack("!I", len(b)) + b


def mfg_blob(unit_id):
    recs = b"harmony!" + rec("unitid") + rec(unit_id) + rec("arch") + rec("0x11")
    img = bytearray(b"\xff" * 0x10000)
    img[0x9000:0x9000 + len(recs)] = recs
    return bytes(img)


def first(replies, sec, rtype):
    for r in replies:
        for x in r[sec]:
            if x["type"] == rtype:
                return x
    return None


def wait_running(hub):
    """Probing (3 x 250 ms) and the two announcements take about 2 s"""
    for _ in range(40):
        if query("127.0.0.1", MDNS_PORT, [(SERVICE, TYPE_PTR, True)], timeout=0.3):
            return True
        time.sleep(0.25)
    return False


def main():
    import tempfile
    with tempfile.NamedTemporaryFile(prefix="mfg_", suffix=".bin", delete=False) as f:
        f.write(mfg_blob(UNIT_ID))
        mfg_path = f.name
    try:
        print("[-] 4.1 responder on port %d" % MDNS_PORT)
        with HubHarness(port=PORT, args=["--mfg-file", mfg_path, "--mdns-port", str(MDNS_PORT)]) as hub:
            check(wait_running(hub), "responder answers after probing and announcing")
            replies = query("127.0.0.1", MDNS_PORT, [(SERVICE, TYPE_PTR, True)], qid=0x1234)
            check(len(replies) == 1, "one reply to the PTR query (%d)" % len(replies))
            r = replies[0] if replies else {"answers": [], "additional": [], "flags": 0, "id": 0}
            check(r["flags"] & 0x8400 == 0x8400 and r["id"] == 0x1234, "response flags and the query id echoed", hex(r["flags"]))
            ptr = first(replies, "answers", TYPE_PTR)
            check(ptr is not None and ptr["name"] == SERVICE and ptr["target"].endswith("." + SERVICE) and ptr["ttl"] == 4500
                  and not ptr["flush"], "PTR in the answers, TTL 4500, no cache flush", str(ptr))
            instance = ptr["target"] if ptr else ""
            check(instance == "Test Hub." + SERVICE, "instance name from hub_name", instance)
            srv = first(replies, "additional", TYPE_SRV)
            txt = first(replies, "additional", TYPE_TXT)
            a = first(replies, "additional", TYPE_A)
            check(srv is not None and srv["name"] == instance and srv["port"] == 8089 and srv["ttl"] == 120 and srv["flush"],
                  "SRV additional: port 8089, TTL 120, cache flush", str(srv))
            hostname = "harmony-ng-%s.local" % UNIT_ID
            check(srv is not None and srv["target"] == hostname, "SRV target is the hub_id hostname", str(srv))
            check(txt is not None and txt["ttl"] == 4500 and txt["flush"], "TXT additional, TTL 4500", str(txt))
            keys = [t.split("=", 1)[0] for t in txt["txt"]] if txt else []
            check(keys == ["id", "ver", "api", "auth", "mqtt", "legacy", "name"], "TXT keys in order (no topic without MQTT)", str(keys))
            vals = dict(t.split("=", 1) for t in txt["txt"]) if txt else {}
            check(vals.get("id") == UNIT_ID and vals.get("api") == "2" and vals.get("auth") == "0" and vals.get("mqtt") == "0"
                  and vals.get("legacy") == "0" and vals.get("name") == "Test Hub" and vals.get("ver", "").count(".") == 2,
                  "TXT values", str(vals))
            check(a is not None and a["name"] == hostname and a["ttl"] == 120 and a["flush"] and a["ip"].count(".") == 3,
                  "A additional for the hostname", str(a))

            # enumeration
            replies = query("127.0.0.1", MDNS_PORT, [("_services._dns-sd._udp.local", TYPE_PTR, True)])
            e = first(replies, "answers", TYPE_PTR)
            check(e is not None and e["target"] == SERVICE, "service enumeration PTR", str(e))
            # instance SRV and TXT, hostname A
            replies = query("127.0.0.1", MDNS_PORT, [(instance, TYPE_SRV, True)])
            check(first(replies, "answers", TYPE_SRV) is not None and first(replies, "additional", TYPE_A) is not None,
                  "SRV query: SRV answer + A additional", str(replies))
            replies = query("127.0.0.1", MDNS_PORT, [(instance, TYPE_TXT, True)])
            check(first(replies, "answers", TYPE_TXT) is not None, "TXT query answered", str(replies))
            replies = query("127.0.0.1", MDNS_PORT, [(hostname, TYPE_A, True)])
            check(first(replies, "answers", TYPE_A) is not None, "A query answered", str(replies))
            replies = query("127.0.0.1", MDNS_PORT, [(instance, TYPE_ANY, True)])
            check(first(replies, "answers", TYPE_SRV) is not None and first(replies, "answers", TYPE_TXT) is not None,
                  "ANY query on the instance: SRV and TXT", str(replies))
            # other names: silence
            replies = query("127.0.0.1", MDNS_PORT, [("_other._tcp.local", TYPE_PTR, True)], timeout=0.7)
            check(replies == [], "no reply for another service type")
            # known-answer suppression: our PTR with a fresh TTL in the query
            known = [(SERVICE, TYPE_PTR, 4500, encode_name(instance))]
            replies = query("127.0.0.1", MDNS_PORT, [(SERVICE, TYPE_PTR, True)], answers=known, timeout=0.7)
            check(replies == [], "known-answer suppression (TTL 4500)")
            known = [(SERVICE, TYPE_PTR, 100, encode_name(instance))]
            replies = query("127.0.0.1", MDNS_PORT, [(SERVICE, TYPE_PTR, True)], answers=known)
            check(first(replies, "answers", TYPE_PTR) is not None, "stale known answer (TTL 100) is answered")

            # name change through the settings
            st, resp = hub.http_post("/api/settings", {"hub_name": "Living/Room: Hub"})
            check(st == 200, "settings POST (%s)" % st)
            found = None
            for _ in range(20):
                replies = query("127.0.0.1", MDNS_PORT, [(SERVICE, TYPE_PTR, True)], timeout=0.5)
                p = first(replies, "answers", TYPE_PTR)
                if p and p["target"] != instance:
                    found = p["target"]
                    break
                time.sleep(0.25)
            check(found == "Living-Room- Hub." + SERVICE, "renamed instance with unsafe characters replaced", str(found))
            txt = first(replies, "additional", TYPE_TXT)
            vals = dict(t.split("=", 1) for t in txt["txt"]) if txt else {}
            check(vals.get("name") == "Living/Room: Hub", "TXT name keeps the raw hub_name", str(vals))

            # mdns_enabled false: port closes; true: back
            st, s = hub.http_get("/api/status")
            check(s.get("mdns_enabled") is True and "mdns" in s.get("capabilities", []), "status: mdns_enabled true, mdns capability", str(s.get("capabilities")))
            st, resp = hub.http_post("/api/settings", {"mdns_enabled": False})
            check(st == 200 and resp.get("mdns_enabled") is False, "settings POST mdns_enabled false", str(resp))
            time.sleep(0.3)
            replies = query("127.0.0.1", MDNS_PORT, [(SERVICE, TYPE_PTR, True)], timeout=0.7)
            check(replies == [], "no reply while disabled")
            import socket
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                probe.bind(("0.0.0.0", MDNS_PORT))
                closed = True
            except OSError:
                closed = False
            probe.close()
            check(closed, "port %d released while disabled" % MDNS_PORT)
            st, s = hub.http_get("/api/status")
            check(s.get("mdns_enabled") is False, "status: mdns_enabled false")
            st, s = hub.http_get("/api/settings")
            check(s.get("mdns_enabled") is False, "GET /api/settings: mdns_enabled false")
            st, resp = hub.http_post("/api/settings", {"mdns_enabled": True})
            check(st == 200 and wait_running(hub), "re-enabled: responder answers again")
    finally:
        os.unlink(mfg_path)
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all mDNS checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
