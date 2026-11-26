"""tools/uboot_mm_load.py against a fake U-Boot `mm` that echoes and prompts like U-Boot 1.1.4."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
import uboot_mm_load as m  # noqa: E402


class FakeMm(m.Console):
    def __init__(self):
        self.log = None
        self.buf = b''
        self.out = bytearray()
        self.mem = {}
        self.addr = None
        self.line = b''

    def send(self, text):
        for c in (text.encode() if isinstance(text, str) else text):
            c = bytes([c])
            if c != b'\r':
                self.line += c
                self.out += c
                continue
            line, self.line = self.line.decode(), b''
            if self.addr is None:
                if line.startswith('mm '):
                    self.addr = int(line[3:], 16)
                    self.out += f'\n{self.addr:08x}: 00000000 ? '.encode()
                else:
                    self.out += b'\nar7240> '
            elif line and all(ch in '0123456789abcdef' for ch in line):
                self.mem[self.addr] = int(line, 16)
                self.addr += 4
                self.out += f'\n{self.addr:08x}: 00000000 ? '.encode()
            else:
                self.addr = None
                self.out += b'\nar7240> '

    def read(self):
        chunk, self.out = bytes(self.out[:7]), self.out[7:]  # trickle like a slow UART
        self.buf += chunk
        return chunk


class TestLoad(unittest.TestCase):
    def check(self, lookahead):
        data = bytes(range(256)) * 3 + b'\x01\x02'  # not a multiple of 4: padded with zeros
        con = FakeMm()
        m.load(con, data, 0x81000000, lookahead)
        padded = data + b'\0\0'
        got = b''.join(con.mem[0x81000000 + i].to_bytes(4, 'big') for i in range(0, len(padded), 4))
        self.assertEqual(got, padded)
        self.assertEqual(len(con.mem), len(padded) // 4)
        self.assertIsNone(con.addr)  # mm was left

    def test_strict(self):
        self.check(0)

    def test_lookahead(self):
        self.check(1)
        self.check(3)


if __name__ == '__main__':
    unittest.main()
