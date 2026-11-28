#!/usr/bin/env python3
"""hub_util usbhid (payload/source/usb_hid_relay.c): the HID gadget's 64-byte reports to and from
hub_util usbwifi on TCP, as the stock usbhid did. A pseudo terminal in raw mode stands in for
/dev/hidg0 (the master end is the PC), a TCP listener for hub_util usbwifi.
"""

from __future__ import annotations

import os
import pathlib
import pty
import select
import socket
import subprocess
import tempfile
import tty
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SRC = REPO_DIR / "payload" / "source" / "usb_hid_relay.c"


def read_exact(fd_or_sock, n: int, timeout: float = 5.0) -> bytes:
    buf = b""
    while len(buf) < n:
        r, _, _ = select.select([fd_or_sock], [], [], timeout)
        if not r:
            raise TimeoutError(f"got {len(buf)} of {n} bytes")
        chunk = fd_or_sock.recv(n - len(buf)) if isinstance(fd_or_sock, socket.socket) else os.read(fd_or_sock, n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


class UsbHidRelay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.bin = pathlib.Path(cls.tmp.name) / "relay"
        main = pathlib.Path(cls.tmp.name) / "main.c"
        main.write_text('#include "usb_hid_relay.h"\nint main(int c, char **v) { return usbhid_relay_main(c, v); }\n')
        subprocess.run(["gcc", "-Wall", "-Wextra", "-O2", f"-I{SRC.parent}", "-o", str(cls.bin), str(main), str(SRC)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.lsn = socket.socket()
        self.lsn.bind(("127.0.0.1", 0))
        self.lsn.listen(2)
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.slave_path = os.ttyname(slave)
        os.close(slave)
        self.proc = subprocess.Popen([str(self.bin), self.slave_path, str(self.lsn.getsockname()[1])],
                                     stderr=subprocess.DEVNULL)

    def tearDown(self):
        self.proc.kill()
        self.proc.wait()
        os.close(self.master)
        self.lsn.close()

    def accept(self) -> socket.socket:
        self.lsn.settimeout(5)
        conn, _ = self.lsn.accept()
        self.assertEqual(read_exact(conn, 1), b"\x06")   # the connection type byte
        return conn

    def test_reports_both_ways_and_reconnect(self):
        conn = self.accept()
        out_report = bytes(range(64))
        os.write(self.master, out_report)                  # the PC sends a report
        self.assertEqual(read_exact(conn, 64), out_report)
        answer = bytes([0xFF, 0x01, 0x81]) + bytes(61)
        conn.sendall(answer[:20])                          # the peer's frame may come in parts
        conn.sendall(answer[20:])
        self.assertEqual(read_exact(self.master, 64), answer)
        conn.close()                                       # hub_util usbwifi restarts
        conn2 = self.accept()
        os.write(self.master, out_report)
        self.assertEqual(read_exact(conn2, 64), out_report)
        conn2.close()


if __name__ == "__main__":
    unittest.main()
