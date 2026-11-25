#!/usr/bin/env python3
"""Plugin datagrams carry the Hub's name ("hub_name"), so a plugin that registers the Hub on its
device (a Samsung TV's list of allowed devices) shows the name the user gave the Hub:
 - a command datagram and a pairing datagram both have it;
 - without a name set, or with the old stored default "Harmony Hub" (the stock Logitech name),
   it is "Harmony-NG Hub";
 - a rename in the settings reaches the next datagram, without a restart.

The harness runs the daemon with --dry-run, which logs a command send instead of sending it
("Plugin command simulated: ... hub=<name>"); the pairing route sends its datagram, received on
the plugin's socket, which the test binds itself (as test_plugin_pair_listen.py does)."""
import json
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, PLUGIN_RUN_DIR  # noqa: E402

PORT = 8953
PID = "namer"
failures = []


def check(cond, name, detail=""):
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, (" (" + detail + ")") if detail and not cond else ""))
    if not cond:
        failures.append(name)


def install(hub):
    d = os.path.join(hub.data_dir, "plugins", PID)
    os.makedirs(os.path.join(d, "bin"), exist_ok=True)
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump({"id": PID, "version": "1.0.0", "entrypoint": "bin/drv",
                   "pairing": {"supported": True, "type": "confirm", "timeout_s": 45}}, f)
    with open(os.path.join(d, "bin", "drv"), "w") as f:
        f.write("#!/bin/sh\nexec sleep 300\n")
    os.chmod(os.path.join(d, "bin", "drv"), 0o755)


class PluginSocket:
    """The plugin's datagram socket, bound again whenever a plugin start removed it."""

    def __init__(self):
        os.makedirs(PLUGIN_RUN_DIR, exist_ok=True)
        self.path = os.path.join(PLUGIN_RUN_DIR, PID + ".sock")
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
                self.got.append(json.loads(s.recv(8192)))
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


def main():
    print("[-] Plugin datagrams carry the Hub's name...")
    log = os.path.join(os.environ.get("TMPDIR", "/tmp"), "test_plugin_hub_name_%d.log" % os.getpid())
    with HubHarness(port=PORT, log_path=log) as hub:
        install(hub)
        sock = PluginSocket()
        st, res = hub.http_post("/ir/new-device", {
            "name": "Plugin TV", "type": "plugin:" + PID, "plugin": PID,
            "config": json.dumps({"host": "192.168.1.70"}),
            "commands": json.dumps([{"name": "Home", "keyCode": "PLUGIN:%s:Home" % PID,
                                     "raw": "PLUGIN:%s:Home" % PID}])})
        dev_id = str(res.get("deviceId") or res.get("id")) if isinstance(res, dict) else ""
        check(st == 200 and dev_id, "plugin device saved", str((st, res)))

        def command_hub():
            """The hub= of the command send the next /api/ir-send logs (dry run)"""
            time.sleep(0.3)   # past the 90 ms debounce of a repeated press of the same command
            with open(log, "rb") as f:
                before = len(f.read())
            st, res = hub.http_post("/api/ir-send", {"deviceId": dev_id, "command": "Home"})
            end = time.time() + 3
            while time.time() < end:
                with open(log, "rb") as f:
                    text = f.read()[before:].decode(errors="replace")
                for line in text.splitlines():
                    if "Plugin command simulated" in line and "cmd=Home" in line and " hub=" in line:
                        return st, line.split(" hub=", 1)[1].split(" sock=", 1)[0]
                time.sleep(0.05)
            return st, None

        def set_name(name):
            st, res = hub.http_post("/api/settings", {"hub_name": name})
            check(st == 200, "settings save hub_name %r" % name, str((st, res)))

        st, name = command_hub()
        check(name == "Test Hub", "a plugin command carries the configured Hub name (sample config: Test Hub)",
              str((st, name)))

        set_name("Harmony Living")
        st, name = command_hub()
        check(name == "Harmony Living", "after a rename the next plugin command has the new name, no restart",
              str((st, name)))

        sock.got.clear()
        st, res = hub.http_post("/api/plugins/pair", {"id": PID, "action": "start", "host": "192.168.1.70"}, timeout=10)
        d = sock.recv()
        check(d.get("action") == "start_pair" and d.get("hub_name") == "Harmony Living",
              "the pairing datagram has the Hub's name too", str((st, res, d)))

        set_name("Harmony Hub")
        st, name = command_hub()
        check(name == "Harmony-NG Hub",
              "the old default name (the stock Logitech one) goes out as Harmony-NG Hub", str((st, name)))
        sock.close()
    if not failures:
        try:
            os.unlink(log)
        except OSError:
            pass

    print("test_plugin_hub_name: %s" % ("all passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
