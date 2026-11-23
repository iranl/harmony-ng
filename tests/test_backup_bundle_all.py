#!/usr/bin/env python3
"""
GET /export/bundle and POST /import with every section a Hub can have: the six remote slot maps,
the slot table (rf_slot_map), hub_id, the Ethernet settings, the Bluetooth map, the web remote
layouts and the Bluetooth keys. The import list once outgrew its array (16 for 21 files) and the
import wrote past it on the stack; the daemon runs with AddressSanitizer here, so that is caught.
Also: text sections stay text (hub_id and the slot table were cut to their first number).
"""

import base64
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import BUILD_DIR, MOCK_LIB, HubHarness, build_hub_daemon_test, build_mock_lib  # noqa: E402

PORT = 8943
ASAN_BIN = os.path.join(BUILD_DIR, "hub_daemon_test_asan")
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def asan_runtime(binary):
    out = subprocess.run(["ldd", binary], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "libasan" in line and "=>" in line:
            return line.split("=>")[1].split()[0]
    return ""


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def main():
    print("[-] Backup bundle with every section: export, import (AddressSanitizer)")
    build_mock_lib()
    build_hub_daemon_test(extra_cflags=["-O1", "-fno-omit-frame-pointer", "-fsanitize=address"], out=ASAN_BIN)
    asan = asan_runtime(ASAN_BIN)
    log_dir = tempfile.mkdtemp(prefix="bundle_asan_")
    env = {
        "LD_PRELOAD": (asan + ":" if asan else "") + MOCK_LIB,
        "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0:log_path=" + os.path.join(log_dir, "asan"),
    }
    with HubHarness(port=PORT, hub_bin=ASAN_BIN, env=env) as hub:
        d = hub.data_dir
        for n in range(1, 7):
            write(os.path.join(d, "remote_slot%d_map.json" % n),
                  json.dumps({"remote": {"name": "Slot %d" % n}, "activities": {}, "devices": {}}))
        write(os.path.join(d, "rf_slot_map.txt"), "1 4 3 2 5 6\n")
        write(os.path.join(d, "hub_id"), "20130507121425-002126\n")
        write(os.path.join(d, "ethernet.conf"), "ETH_ENABLED=1\n")
        write(os.path.join(d, "bt_remote_map.json"), json.dumps({"remote": {"name": "BT"}, "activities": {}}))
        write(os.path.join(d, "web_remote_layouts.json"), json.dumps({"layouts": {}}))
        with open(os.path.join(d, "btstack_keys.tlv"), "wb") as f:
            f.write(b"\x01\x02\x03keys")

        st, bundle = hub.http_get("/export/bundle")
        check(st == 200 and isinstance(bundle, dict), "bundle exported", str(st))
        bundle = bundle if isinstance(bundle, dict) else {}
        for n in range(1, 7):
            check(isinstance(bundle.get("remote_slot%d_map" % n), dict), "slot %d map in the bundle" % n)
        check(bundle.get("rf_slot_map") == "1 4 3 2 5 6\n", "slot table exported as text", repr(bundle.get("rf_slot_map")))
        check(bundle.get("hub_id") == "20130507121425-002126\n", "hub_id exported as text", repr(bundle.get("hub_id")))
        check(bool(bundle.get("btstack_keys")), "Bluetooth keys in the bundle")

        # change what the import must put back
        write(os.path.join(d, "remote_slot6_map.json"), json.dumps({"remote": {"name": "changed"}}))
        write(os.path.join(d, "rf_slot_map.txt"), "1 2 3 4 5 6\n")
        st, r = hub.http_post("/import", {"target": "bundle", "payload": bundle, "restore_network": True}, timeout=15)
        check(st == 200, "bundle with every section imported", "%s %s" % (st, r))
        st2, _ = hub.http_get("/api/status")
        check(st2 == 200, "daemon still answers after the import", str(st2))
        with open(os.path.join(d, "remote_slot6_map.json")) as f:
            check(json.load(f).get("remote", {}).get("name") == "Slot 6", "slot 6 map restored")
        with open(os.path.join(d, "rf_slot_map.txt")) as f:
            check(f.read() == "1 4 3 2 5 6\n", "slot table restored")

        # a bundle from before text sections stayed text: hub_id as a number is skipped
        old = dict(bundle)
        old["hub_id"] = 20130507121425
        st, r = hub.http_post("/import", {"target": "bundle", "payload": old}, timeout=15)
        check(st == 200, "old bundle imported", "%s %s" % (st, r))
        with open(os.path.join(d, "hub_id")) as f:
            check(f.read() == "20130507121425-002126\n", "a cut numeric hub_id is not restored")

    reports = [f for f in os.listdir(log_dir) if f.startswith("asan")]
    detail = ""
    for f in reports:
        with open(os.path.join(log_dir, f)) as fh:
            detail += fh.read()[:2000]
    check(not reports, "no AddressSanitizer report", detail)
    if failed:
        print("[-] FAIL: %d bundle check(s) failed" % failed)
        sys.exit(1)
    print("[+] Backup bundle with every section: all checks passed")


if __name__ == "__main__":
    main()
