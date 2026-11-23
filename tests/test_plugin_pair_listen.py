#!/usr/bin/env python3
"""
POST /api/plugins/pair without a host (a "listen" pairing, PR #243 review): the route returns
at once, action "status" reads the plugin's status file, and only config keys the manifest's
config_fields declare reach the UI. A PIN pairing still needs a host; a "confirm" pairing (the
user accepts a prompt on the device) needs a host and returns at once like listen.

The harness cannot run a plugin (its file mock does not cover chdir), so the test writes the
status file the plugin would write.
"""

import json
import os
import socket
import threading
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


class PluginSocket:
    """The datagram socket a running plugin listens on (the route sends the pairing there). The
    harness cannot run the plugin, and starting one removes its old socket, so this binds it
    again whenever it is gone, the way a plugin binds its socket once it runs."""

    def __init__(self, pid):
        os.makedirs(PLUGIN_RUN_DIR, exist_ok=True)
        self.path = os.path.join(PLUGIN_RUN_DIR, pid + ".sock")
        self.got = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        s = None
        while not self.stop.is_set():
            if s is None or not os.path.exists(self.path):
                if s:
                    s.close()
                try:
                    os.unlink(self.path)
                except OSError:
                    pass
                s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
                s.bind(self.path)
                s.settimeout(0.05)
            try:
                self.got.append(json.loads(s.recv(4096)))
            except (socket.timeout, ValueError):
                pass
        s.close()

    def recv(self, timeout=3):
        end = time.time() + timeout
        while not self.got and time.time() < end:
            time.sleep(0.05)
        return self.got.pop(0) if self.got else {"error": "timed out"}

    def close(self):
        self.stop.set()
        self.thread.join()


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
        install(hub, "confirmer", {
            "id": "confirmer", "version": "1.0.0", "entrypoint": "bin/drv",
            "pairing": {"supported": True, "type": "confirm", "timeout_s": 45},
        })
        install(hub, "pinner", {
            "id": "pinner", "version": "1.0.0", "entrypoint": "bin/drv",
            "pairing": {"supported": True, "type": "pin_display"},
        })
        install(hub, "ghost", {
            "id": "ghost", "version": "1.0.0", "entrypoint": "bin/drv",
            "pairing": {"supported": True, "type": "listen", "requires_host": False},
        })
        try:
            os.unlink(STATUS)
        except OSError:
            pass
        lsock = PluginSocket("listener")
        csock = PluginSocket("confirmer")

        st, res = hub.http_post("/api/plugins/pair", {"id": "listener", "action": "status"})
        check(st == 200 and res.get("stage") == "waiting", "status before any pairing: waiting", str((st, res)))

        t0 = time.time()
        st, res = hub.http_post("/api/plugins/pair", {"id": "listener", "action": "start"}, timeout=10)
        took = time.time() - t0
        check(st == 200 and res.get("stage") == "listening" and res.get("timeout_s") == 20,
              "listen pairing starts without a host and answers 'listening'", str((st, res)))
        check(took < 3, "the route does not wait for the pairing to end (%.1f s)" % took)
        dgram = lsock.recv()
        check(dgram.get("action") == "start_pair" and "host" not in dgram and dgram.get("timeout_s") == 20,
              "the plugin gets start_pair without a host, with timeout_s", str(dgram))
        st, res = hub.http_post("/api/plugins/pair", {"id": "ghost", "action": "start"}, timeout=10)
        check(st == 400 and res.get("stage") == "error",
              "a plugin that cannot take the pairing (no socket) is an error, not 'listening'", str((st, res)))

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

        # confirm: the user accepts a prompt on the device; a host is needed, no PIN, polled
        st, res = hub.http_post("/api/plugins/pair", {"id": "confirmer", "action": "start"})
        check(st == 400, "a confirm pairing needs a host", str((st, res)))
        t0 = time.time()
        st, res = hub.http_post("/api/plugins/pair", {"id": "confirmer", "action": "start", "host": "192.168.1.70"}, timeout=10)
        check(st == 200 and res.get("stage") == "confirm_on_device" and res.get("timeout_s") == 45,
              "confirm pairing answers at once: confirm_on_device", str((st, res)))
        check(time.time() - t0 < 3, "the confirm route does not wait for the user")
        lsock.close()
        csock.close()
        cstatus = os.path.join(PLUGIN_RUN_DIR, "confirmer.status")
        with open(cstatus + ".tmp", "w") as f:
            json.dump({"ok": True, "action": "start_pair", "stage": "done", "message": "accepted"}, f)
        os.rename(cstatus + ".tmp", cstatus)
        st, res = hub.http_post("/api/plugins/pair", {"id": "confirmer", "action": "status"})
        check(st == 200 and res.get("stage") == "done", "confirm status polls reach done", str((st, res)))
        st, res = hub.http_post("/api/plugins/pair", {"id": "../x", "action": "status"})
        check(st == 400, "unsafe id refused for status", str((st, res)))

    if failed:
        print("[-] FAIL: %d plugin pairing check(s) failed" % failed)
        sys.exit(1)
    print("[+] Plugin pairing without a host: all checks passed")


if __name__ == "__main__":
    main()
