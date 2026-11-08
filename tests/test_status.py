#!/usr/bin/env python3
"""
GET /api/status identity fields (spec 3.6): hub_id from the mfg blob the harness was started
with (--mfg-file), version, commit, api_version 2, capabilities, legacy_enabled, mdns_enabled.
"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8996
UNIT_ID = "20140101120000-000001"
# Flags of milestone 1 (spec 2.2)
M1_CAPABILITIES = ["events", "rf_wake"]


def rec(s):
    b = s.encode("ascii") if isinstance(s, str) else s
    return struct.pack("!I", len(b)) + b


def mfg_blob(unit_id):
    """A 64 KB partition image: 0xFF, records at 0x9000 as on the Hub"""
    recs = b"harmony!" + rec("unitid") + rec(unit_id) + rec("arch") + rec("0x11") + rec("skin") + rec("0x61") \
        + rec("usb_vendor_id") + rec("0x046D") + rec("hw_ver") + rec("01.00") + rec("key") + rec(bytes(range(256)) * 4)
    img = bytearray(b"\xff" * 0x10000)
    img[0x9000:0x9000 + len(recs)] = recs
    return bytes(img)


def main():
    failed = 0

    def check(cond, name, detail=""):
        nonlocal failed
        if cond:
            print("  PASS %s" % name)
        else:
            failed += 1
            print("  FAIL %s %s" % (name, detail))

    with tempfile.NamedTemporaryFile(prefix="mfg_", suffix=".bin", delete=False) as f:
        f.write(mfg_blob(UNIT_ID))
        mfg_path = f.name
    try:
        print("[-] 3.6 identity, version and capabilities in GET /api/status")
        with HubHarness(port=PORT, args=["--mfg-file", mfg_path]) as hub:
            st, s = hub.http_get("/api/status")
            check(st == 200 and s.get("ok") is True, "GET /api/status", "%s %s" % (st, s))
            check(s.get("hub_id") == UNIT_ID, "hub_id is the unit id of the mfg blob", str(s.get("hub_id")))
            check(isinstance(s.get("version"), str) and s["version"].count(".") == 2, "version", str(s.get("version")))
            check(isinstance(s.get("commit"), str) and s["commit"], "commit", str(s.get("commit")))
            check(s.get("api_version") == 2, "api_version 2", str(s.get("api_version")))
            caps = s.get("capabilities")
            check(isinstance(caps, list) and all(c in caps for c in M1_CAPABILITIES),
                  "capabilities contain %s" % M1_CAPABILITIES, str(caps))
            check(s.get("legacy_enabled") is False and s.get("mdns_enabled") is False, "legacy_enabled and mdns_enabled false",
                  str((s.get("legacy_enabled"), s.get("mdns_enabled"))))
            check(s.get("firmware") == "4.15.600" and "hub_name" in s and "uptime_sec" in s, "existing fields kept", str(s))
        print("[-] 3.6 fallback without a readable mfg partition")
        with HubHarness(port=PORT, args=["--mfg-file", "/nonexistent/mfg.bin"]) as hub:
            st, s = hub.http_get("/api/status")
            hid = s.get("hub_id") if isinstance(s, dict) else None
            check(st == 200 and isinstance(hid, str) and hid and hid != UNIT_ID, "hub_id falls back (MAC or unknown)", str(hid))
    finally:
        os.unlink(mfg_path)
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all status checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
