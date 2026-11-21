#!/usr/bin/env python3
"""
POST /api/plugins/pair without a host (a "listen" pairing, PR #243 review): the route returns
at once, action "status" reads the plugin's status file, and only config keys the manifest's
config_fields declare reach the UI. A plugin without a listen pairing still needs a host.

The harness cannot run a plugin (its file mock does not cover chdir), so the test writes the
status file the plugin would write.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, PLUGIN_RUN_DIR

PORT = 8947
failed = 0
STATUS = os.path.join(PLUGIN_RUN_DIR, "listener.status")


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def install(hub, pid, manifest):
    d = os.path.join(hub.data_dir, "plugins", pid)
    os.makedirs(os.path.join(d, "bin"), exist_ok=True)
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(manifest, f)
    with open(os.path.join(d, "bin", "drv"), "w") as f:
        f.write("#!/bin/sh\nexec sleep 300\n")
    os.chmod(os.path.join(d, "bin", "drv"), 0o755)


def write_status(obj):
    os.makedirs(PLUGIN_RUN_DIR, exist_ok=True)
    with open(STATUS + ".tmp", "w") as f:
        json.dump(obj, f)
    os.rename(STATUS + ".tmp", STATUS)


def main():
    print("[-] Plugin pairing without a host (listen)...")
    with HubHarness(port=PORT) as hub:
        install(hub, "listener", {
            "id": "listener", "version": "1.0.0", "entrypoint": "bin/drv",
            "pairing": {"supported": True, "type": "listen", "requires_host": False, "timeout_s": 20},
            "device_definition": {"config_fields": [
                {"key": "switch_mac", "type": "string"}, {"key": "controller_mac", "type": "string"},
                {"key": "controller_pid", "type": "string"}]},
        })
        install(hub, "pinner", {
            "id": "pinner", "version": "1.0.0", "entrypoint": "bin/drv",
            "pairing": {"supported": True, "type": "pin_display"},
        })
        try:
            os.unlink(STATUS)
        except OSError:
            pass

        st, res = hub.http_post("/api/plugins/pair", {"id": "listener", "action": "status"})
        check(st == 200 and res.get("stage") == "waiting", "status before any pairing: waiting", str((st, res)))

        t0 = time.time()
        st, res = hub.http_post("/api/plugins/pair", {"id": "listener", "action": "start"}, timeout=10)
        took = time.time() - t0
        check(st == 200 and res.get("stage") == "listening" and res.get("timeout_s") == 20,
              "listen pairing starts without a host and answers 'listening'", str((st, res)))
        check(took < 3, "the route does not wait for the pairing to end (%.1f s)" % took)

        write_status({"ok": True, "action": "start_pair", "stage": "done", "message": "found",
                      "config": {"switch_mac": "98:E2:55:99:1F:97", "controller_pid": "2066",
                                 "evil": "not a config field", "controller_mac": {"nested": 1}}})
        st, res = hub.http_post("/api/plugins/pair", {"id": "listener", "action": "status"})
        cfg = res.get("config", {})
        check(st == 200 and res.get("stage") == "done", "status reads the plugin's answer", str((st, res)))
        check(cfg.get("switch_mac") == "98:E2:55:99:1F:97" and cfg.get("controller_pid") == "2066",
              "learned config values returned", str(cfg))
        check("evil" not in cfg, "a key outside config_fields is dropped", str(cfg))
        check("controller_mac" not in cfg, "a value that is not a string, number or bool is dropped", str(cfg))

        write_status({"ok": False, "action": "start_pair", "stage": "error", "message": "nothing heard"})
        st, res = hub.http_post("/api/plugins/pair", {"id": "listener", "action": "status"})
        check(st == 400 and res.get("stage") == "error", "a failed pairing answers 400", str((st, res)))

        st, res = hub.http_post("/api/plugins/pair", {"id": "pinner", "action": "start"})
        check(st == 400, "a PIN pairing still needs a host", str((st, res)))
        st, res = hub.http_post("/api/plugins/pair", {"id": "../x", "action": "status"})
        check(st == 400, "unsafe id refused for status", str((st, res)))

    if failed:
        print("[-] FAIL: %d plugin pairing check(s) failed" % failed)
        sys.exit(1)
    print("[+] Plugin pairing without a host: all checks passed")


if __name__ == "__main__":
    main()
