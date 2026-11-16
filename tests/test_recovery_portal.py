#!/usr/bin/env python3
"""The Wi-Fi recovery portal (hub_util portal) on the host.

hub_util is built for the host with -DTEST_HARNESS: the portal listens on 127.0.0.1, writes
its wpa_supplicant file to HARMONY_PORTAL_WPA_CONF and never reboots. Checks:
- SIGPIPE is ignored (a client that goes away before the answer must not end the portal);
- a client that connects and sends nothing does not hold the portal: another client is
  answered while it is still connected;
- values wpa_supplicant cannot take (a pass phrase of 5 characters, a line break) bring the
  form back and write nothing;
- a POST whose body arrives in a second segment is read whole: the values are saved as typed
  (no backslashes added) and the "saved" page is sent.
"""

from __future__ import annotations

import os
import pathlib
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.parse

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SRC = REPO_DIR / "payload" / "source"
PORT = int(os.environ.get("PORTAL_TEST_PORT", "9141"))


def hub_util_sources() -> list[str]:
    """hub_util's sources as build/compile_all.sh lists them (the list may grow)"""
    for line in (REPO_DIR / "build" / "compile_all.sh").read_text().splitlines():
        if '-o "$BIN/hub_util"' in line:
            return [str(REPO_DIR / t) for t in line.split() if t.startswith("payload/source/") and t.endswith(".c")]
    raise RuntimeError("hub_util build line not found in build/compile_all.sh")


def build_hub_util(out: pathlib.Path) -> None:
    cmd = ["gcc", "-Wall", "-O2", "-DTEST_HARNESS", f"-I{SRC}", "-o", str(out), *hub_util_sources(), "-lm"]
    subprocess.run(cmd, check=True)


def connect() -> socket.socket:
    deadline = time.time() + 5
    while True:
        try:
            return socket.create_connection(("127.0.0.1", PORT), timeout=10)
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)


def read_all(s: socket.socket) -> bytes:
    data = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    return data


def post_body(**fields: str) -> bytes:
    return urllib.parse.urlencode(fields).encode()


class RecoveryPortal(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls.tmp.name)
        cls.bin = base / "hub_util"
        build_hub_util(cls.bin)
        cls.wpa = base / "wpa_supplicant.conf"
        env = dict(os.environ, HARMONY_PORTAL_WPA_CONF=str(cls.wpa))
        cls.proc = subprocess.Popen([str(cls.bin), "portal", str(PORT)], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        connect().close()

    @classmethod
    def tearDownClass(cls):
        if cls.proc.poll() is None:
            cls.proc.kill()
        cls.proc.wait()
        cls.tmp.cleanup()

    def test_1_sigpipe_ignored(self):
        status = pathlib.Path(f"/proc/{self.proc.pid}/status").read_text()
        sig_ign = int(next(l for l in status.splitlines() if l.startswith("SigIgn:")).split()[1], 16)
        self.assertTrue(sig_ign & (1 << (13 - 1)), "SIGPIPE must be ignored by the portal")

    def test_2_idle_client_does_not_block(self):
        idle = connect()  # sends nothing
        try:
            time.sleep(0.2)
            c = connect()
            c.settimeout(8)
            c.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            started = time.time()
            answer = read_all(c)
            c.close()
            self.assertIn(b"Harmony Hub Setup", answer)
            self.assertLess(time.time() - started, 7.5)
        finally:
            idle.close()
        self.assertIsNone(self.proc.poll())

    def send_post(self, body: bytes, split: bool) -> bytes:
        c = connect()
        c.settimeout(8)
        head = (f"POST /wifi HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                f"Content-Length: {len(body)}\r\n\r\n").encode()
        if split:
            c.sendall(head)
            time.sleep(0.3)
            c.sendall(body)
        else:
            c.sendall(head + body)
        answer = read_all(c)
        c.close()
        return answer

    def test_3_invalid_values_show_the_form(self):
        for body in (post_body(ssid="Home", psk="short"), post_body(ssid="Home\nnetwork={", psk="password123")):
            answer = self.send_post(body, split=False)
            self.assertIn(b"Harmony Hub Setup", answer)
            self.assertNotIn(b"Wi-Fi Saved", answer)
            self.assertFalse(self.wpa.exists())
        self.assertIsNone(self.proc.poll())

    def test_4_body_in_second_segment_is_saved_as_typed(self):
        answer = self.send_post(post_body(ssid='My "Home"', psk="Pa$$word`2026\\"), split=True)
        self.assertIn(b"Wi-Fi Saved", answer)
        conf = self.wpa.read_text()
        self.assertIn('ssid="My "Home""', conf)
        self.assertIn('psk="Pa$$word`2026\\"', conf)
        # the portal ends after a save (the Hub reboots there)
        self.proc.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
