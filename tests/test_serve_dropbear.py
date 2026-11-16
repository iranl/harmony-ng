#!/usr/bin/env python3
"""tools/unbrick_recovery/serve_dropbear.py hands the file only to the Hub.

The server runs on the loopback address with a port of this test group. A host that is
not the Hub (127.0.0.2 here) connects first: it must get nothing, and the server must keep
waiting until the Hub (127.0.0.1) has the whole file. No other network.
"""

from __future__ import annotations

import hashlib
import importlib.util
import os
import pathlib
import socket
import threading
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
PORT = 9220


def load_tool():
    spec = importlib.util.spec_from_file_location("serve_dropbear", REPO_DIR / "tools" / "unbrick_recovery" / "serve_dropbear.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fetch(source: str) -> bytes:
    s = socket.socket()
    s.bind((source, 0))
    s.settimeout(10)
    s.connect(("127.0.0.1", PORT))
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    return buf


class ServeDropbear(unittest.TestCase):
    def test_only_the_hub_gets_the_file(self):
        mod = load_tool()
        data = os.urandom(200_000)
        ready = threading.Event()
        result: dict[str, object] = {}

        def run():
            result["peer"] = mod.serve(data, "127.0.0.1", PORT, hub="127.0.0.1", log=lambda *_: None, ready=ready.set)

        t = threading.Thread(target=run, daemon=True)
        t.start()
        self.assertTrue(ready.wait(5))
        self.assertEqual(fetch("127.0.0.2"), b"", "a host other than the Hub got data")
        self.assertTrue(t.is_alive(), "the server stopped after a connection from another host")
        got = fetch("127.0.0.1")
        t.join(10)
        self.assertEqual(hashlib.md5(got).hexdigest(), hashlib.md5(data).hexdigest())
        self.assertEqual(result.get("peer"), "127.0.0.1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
