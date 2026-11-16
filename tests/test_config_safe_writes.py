#!/usr/bin/env python3
"""
Config saves on a full flash, and config files replaced instead of rewritten in place.

hub_daemon_test runs with a file size limit (RLIMIT_FSIZE, SIGXFSZ ignored) standing in for a
/data partition that runs out of space: every write past the limit fails with EFBIG, as a write
to a full JFFS2 fails with ENOSPC. The saves used to print a cut file over the good one and answer
success. Checks:

  - a device save, a commands batch and an activity save whose file does not fit answer 500
    (the batch with count 0), the file keeps its old bytes, and the daemon still serves the old
    device;
  - a mapping save and a remote layout save that do not fit answer 500 and leave the old file;
  - small saves (settings) still work under the same limit, and no temporary file is left;
  - ethernet.conf is replaced by a new file (new inode), not truncated and rewritten in place.
"""

import json
import os
import stat
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HUB_BIN, HubHarness, build_hub_daemon_test  # noqa: E402

PORT = 8955
FSIZE_LIMIT = 3000  # bytes: the sample devices.json (about 4 KB) no longer fits, settings.json does
failed = 0

WRAPPER = """#!%s
import os, resource, signal, sys
signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
resource.setrlimit(resource.RLIMIT_FSIZE, (%d, %d))
os.execv(%r, [%r] + sys.argv[1:])
"""


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def leftovers(hub):
    out = []
    for root, _, files in os.walk(hub.data_dir):
        out += [os.path.join(root, f) for f in files if ".tmp" in f]
    return out


def full_flash_saves(hub_bin):
    print("[-] Saves that do not fit on the flash")
    with HubHarness(port=PORT, hub_bin=hub_bin) as hub:
        dev_path = os.path.join(hub.data_dir, "config", "devices.json")
        map_path = os.path.join(hub.data_dir, "remote_slot1_map.json")
        layout_path = os.path.join(hub.data_dir, "web_remote_layouts.json")
        dev_before = read(dev_path)
        map_before = read(map_path)
        check(dev_before is not None and len(dev_before) > FSIZE_LIMIT, "sample devices.json larger than the limit")

        st, r = hub.http_post("/api/device-save", {"deviceId": "1", "name": "Renamed on a full flash"})
        check(st == 500 and isinstance(r, dict) and r.get("ok") is False,
              "device save answers 500 (a storage failure, not a bad request)", "%s %s" % (st, r))
        check(read(dev_path) == dev_before, "devices.json keeps its old bytes",
              "%d bytes now" % len(read(dev_path) or b""))
        st, inv = hub.http_get("/api/inventory")
        names = json.dumps(inv) if st == 200 else ""
        check("Renamed on a full flash" not in names, "daemon still serves the old device", str(st))

        st, r = hub.http_post("/api/commands-batch", {"deviceId": "1", "commands": [
            {"name": "FullFlash%d" % i, "raw": "0000 006D 0002 0000 0016 0041 0016 0041"} for i in range(3)]})
        check(st == 500 and isinstance(r, dict) and r.get("ok") is False and r.get("count") == 0,
              "commands batch answers 500 and count 0 when nothing was saved", "%s %s" % (st, r))
        st, r = hub.http_post("/api/command-save", {"deviceId": "1", "name": "FullFlashOne",
                                                     "raw": "0000 006D 0002 0000 0016 0041 0016 0041"})
        check(st == 500 and isinstance(r, dict) and r.get("ok") is False,
              "command save answers 500 when devices.json does not fit", "%s %s" % (st, r))
        st, r = hub.http_post("/api/command-delete", {"deviceId": "1", "command": "PowerOn"})
        check(st == 500 and isinstance(r, dict) and r.get("ok") is False,
              "command delete answers 500 when devices.json cannot be written", "%s %s" % (st, r))
        check(read(dev_path) == dev_before, "devices.json still keeps its old bytes")

        act_path = os.path.join(hub.data_dir, "config", "activities.json")
        act_before = read(act_path)
        steps = [{"type": "Command", "deviceId": "1", "command": "PowerOn"} for _ in range(80)]
        st, r = hub.http_post("/api/activity-save", {"id": "1", "name": "Too big", "startSequence": steps})
        check(st == 500 and isinstance(r, dict) and r.get("ok") is False,
              "activity save answers 500 when activities.json does not fit", "%s %s" % (st, r))
        check(read(act_path) == act_before, "activities.json keeps its old bytes")

        big_map = json.dumps({"activities": {}, "devices": {}, "pad": "x" * (2 * FSIZE_LIMIT)})
        st, r = hub.http_post("/api/rf-mapping-save", None, raw=big_map.encode())
        check(st == 500, "mapping save answers 500", "%s %s" % (st, r))
        check(read(map_path) == map_before, "remote_slot1_map.json keeps its old bytes")

        big_layout = json.dumps({"layouts": ["y" * (2 * FSIZE_LIMIT)]})
        st, r = hub.http_post("/api/ui-remote-layout", None, raw=big_layout.encode())
        check(st == 500, "layout save answers 500", "%s %s" % (st, r))
        check(read(layout_path) is None, "no cut web_remote_layouts.json", "%d bytes" % len(read(layout_path) or b""))

        st, r = hub.http_post("/api/settings", {"hub_name": "Small Save"})
        check(st == 200 and isinstance(r, dict) and r.get("ok") is True, "small save still works", "%s %s" % (st, r))
        settings = json.loads(read(os.path.join(hub.data_dir, "config", "settings.json")) or b"{}")
        check(settings.get("hub_name") == "Small Save", "settings.json written")
        check(leftovers(hub) == [], "no temporary file left", str(leftovers(hub)))


def replaced_not_rewritten():
    print("[-] ethernet.conf replaced through a temporary file")
    old = "ETH_ENABLED=0\n"
    with HubHarness(port=PORT, extra_data_files={"ethernet.conf": old}) as hub:
        path = os.path.join(hub.data_dir, "ethernet.conf")
        ino_before = os.stat(path)[stat.ST_INO]
        st, r = hub.http_post("/api/ethernet-save", {"eth_enabled": True, "eth_mode": "dhcp"})
        check(st == 200 and isinstance(r, dict) and r.get("ok") is True, "ethernet settings saved", "%s %s" % (st, r))
        text = (read(path) or b"").decode()
        check("ETH_ENABLED=1" in text, "new content in ethernet.conf")
        check(os.stat(path)[stat.ST_INO] != ino_before, "ethernet.conf is a new file, not truncated in place")
        check(leftovers(hub) == [], "no temporary file left", str(leftovers(hub)))


def main():
    build_hub_daemon_test()
    with tempfile.TemporaryDirectory() as tmp:
        wrapper = os.path.join(tmp, "hub_daemon_fsize")
        with open(wrapper, "w") as f:
            f.write(WRAPPER % (sys.executable, FSIZE_LIMIT, FSIZE_LIMIT, HUB_BIN, HUB_BIN))
        os.chmod(wrapper, 0o755)
        full_flash_saves(wrapper)
    replaced_not_rewritten()
    if failed:
        print("config safe writes: %d FAILED" % failed)
        sys.exit(1)
    print("all config safe write checks passed")


if __name__ == "__main__":
    main()
