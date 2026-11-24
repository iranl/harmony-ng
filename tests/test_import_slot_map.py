#!/usr/bin/env python3
"""
The slot table (rf_slot_map) in a backup import. It must be six different pipes from 1 to 6, the
check hub_rf makes at its start: any other text used to become the file (hub_rf then dropped the
table, and the remotes of swapped slots changed places). hub_rf restarts after the import only
when the table changes: a restart takes the remotes off the air, and a full restore of an
unchanged Hub asked for one every time. Also: rf_slot_map as a single import and export target
(API.md 9.1 lists it), and a bundle leaves out a table file that is no table.
"""

import json
import os
import socket
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_hub_daemon_test, build_mock_lib  # noqa: E402

PORT = 9610
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


class FakeHubRf:
    """hub_rf's loopback API port (8092, as hub_cmd.c): counts the restart requests"""

    def __init__(self):
        self.lines = []
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.srv.bind(("127.0.0.1", 8092))
        except OSError:
            self.srv.close()
            self.srv = None
            return
        self.srv.listen(8)
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            with c:
                c.settimeout(2)
                try:
                    self.lines.append(c.recv(4096).split(b"\r\n", 1)[0].decode())
                    c.sendall(b"HTTP/1.1 202 Accepted\r\nConnection: close\r\n\r\n{\"ok\":true}")
                except OSError:
                    pass

    def restarts(self):
        n = sum(1 for line in self.lines if line.startswith("POST /api/rf/restart "))
        self.lines.clear()
        return n

    def close(self):
        if self.srv:
            self.srv.close()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def read(path):
    with open(path) as f:
        return f.read()


def main():
    print("[-] Backup import: the RF slot table is checked, and hub_rf restarts only when it changes")
    build_mock_lib()
    build_hub_daemon_test()
    rf = FakeHubRf()
    if not rf.srv:
        print("  SKIP port 8092 in use: the restart requests cannot be counted")
    with HubHarness(port=PORT) as hub:
        d = hub.data_dir
        table = os.path.join(d, "rf_slot_map.txt")
        write(table, "1 4 3 2 5 6\n")
        st, bundle = hub.http_get("/export/bundle")
        check(st == 200 and isinstance(bundle, dict) and bundle.get("rf_slot_map") == "1 4 3 2 5 6\n",
              "bundle carries the table", repr(bundle.get("rf_slot_map") if isinstance(bundle, dict) else bundle))
        bundle = bundle if isinstance(bundle, dict) else {}

        for label, value in (("not a table", "banana\n"), ("a pipe twice", "1 1 3 4 5 6\n"),
                             ("five pipes", "1 2 3 4 5\n"), ("a pipe 7", "1 2 3 4 5 7\n"),
                             ("a number", 1)):
            rf.lines.clear()
            st, r = hub.http_post("/import", {"target": "bundle", "payload": dict(bundle, rf_slot_map=value)}, timeout=15)
            check(st == 400 and "rf_slot_map" in json.dumps(r), "bundle with %s is refused" % label, "%s %s" % (st, r))
            check(read(table) == "1 4 3 2 5 6\n", "the table file is unchanged (%s)" % label, repr(read(table)))
            if rf.srv:
                check(rf.restarts() == 0, "hub_rf not restarted (%s)" % label)

        # the live table again (other spacing): nothing for hub_rf to do
        st, r = hub.http_post("/import", {"target": "bundle", "payload": dict(bundle, rf_slot_map="1  4 3 2 5 6")}, timeout=15)
        check(st == 200, "bundle with the live table imports", "%s %s" % (st, r))
        if rf.srv:
            check(rf.restarts() == 0, "the live table does not restart hub_rf")

        st, r = hub.http_post("/import", {"target": "bundle", "payload": dict(bundle, rf_slot_map="2 1 3 4 5 6\n")}, timeout=15)
        check(st == 200 and read(table) == "2 1 3 4 5 6\n", "another table imports", "%s %s %r" % (st, r, read(table)))
        if rf.srv:
            check(rf.restarts() == 1, "a changed table restarts hub_rf once")

        # no table file on the Hub = slots and pipes the same
        os.unlink(table)
        st, r = hub.http_post("/import", {"target": "bundle", "payload": dict(bundle, rf_slot_map="1 2 3 4 5 6\n")}, timeout=15)
        check(st == 200, "the plain table imports on a Hub without a table file", "%s %s" % (st, r))
        if rf.srv:
            check(rf.restarts() == 0, "...without a restart (hub_rf already uses it)")

        # single target, both ways
        st, r = hub.http_post("/import", {"target": "rf_slot_map", "payload": "1 4 3 2 5 6"}, timeout=15)
        check(st == 200 and read(table) == "1 4 3 2 5 6\n", "rf_slot_map imports as a single target", "%s %s" % (st, r))
        if rf.srv:
            check(rf.restarts() == 1, "...and restarts hub_rf")
        st, r = hub.http_post("/import", {"target": "rf_slot_map", "payload": "1 2 2 4 5 6"}, timeout=15)
        check(st == 400 and read(table) == "1 4 3 2 5 6\n", "a single target that is no table is refused", "%s %s" % (st, r))
        st, body = hub.http_get("/export/rf_slot_map")
        check(st == 200 and str(body).strip() == "1 4 3 2 5 6", "rf_slot_map exports as a single target", "%s %r" % (st, body))

        # a table file that is no table stays out of the bundle (its import would be refused)
        write(table, "banana\n")
        st, b2 = hub.http_get("/export/bundle")
        check(st == 200 and isinstance(b2, dict) and "rf_slot_map" not in b2, "a file that is no table is not exported",
              repr(b2.get("rf_slot_map") if isinstance(b2, dict) else b2))
        st, r = hub.http_post("/import", {"target": "bundle", "payload": b2}, timeout=15)
        check(st == 200, "that bundle imports again", "%s %s" % (st, r))
    rf.close()

    if failed:
        print("[-] test_import_slot_map: %d FAILED" % failed)
        return 1
    print("[+] test_import_slot_map passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
