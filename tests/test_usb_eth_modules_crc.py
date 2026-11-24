#!/usr/bin/env python3
"""Every module in payload/modules must load on the stock Hub kernel (modversions).

The kernel refuses a module that records no CRC for a symbol it uses ("no symbol version for
usb_clear_halt", "Unknown symbol ..."): usbnet.ko, smsc95xx.ko and others were shipped that way.
The test reads each .ko (big endian ELF32) and checks that __versions names every undefined
symbol. build/build_usb_ethernet.sh makes the same check before it copies a module.
"""

from __future__ import annotations

import pathlib
import struct
import sys
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
MODULES = REPO_DIR / "payload" / "modules"
sys.path.insert(0, str(REPO_DIR / "build"))
import extract_symvers  # noqa: E402


def undefined_symbols(ko: pathlib.Path) -> set[str]:
    b = ko.read_bytes()
    shoff, = struct.unpack(">I", b[32:36])
    shentsize, shnum = struct.unpack(">HH", b[46:50])
    secs = [struct.unpack(">IIIIIIIIII", b[shoff + i * shentsize: shoff + i * shentsize + 40]) for i in range(shnum)]
    out = set()
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]  # sh_link
        for o in range(s[4], s[4] + s[5], 16):
            name, _value, _size, info, _other, shndx = struct.unpack(">IIIBBH", b[o:o + 16])
            if shndx == 0 and name and (info >> 4) in (1, 2):  # SHN_UNDEF, GLOBAL or WEAK
                start = strtab[4] + name
                out.add(b[start:b.index(b"\x00", start)].decode())
    return out


class ModuleCrcTest(unittest.TestCase):
    def test_every_symbol_has_a_crc(self):
        kos = sorted(MODULES.glob("*.ko"))
        self.assertTrue(kos)
        for ko in kos:
            with self.subTest(module=ko.name):
                missing = undefined_symbols(ko) - set(extract_symvers.module_versions(ko))
                self.assertFalse(missing, "no CRC recorded for %s" % ", ".join(sorted(missing)))


if __name__ == "__main__":
    unittest.main()
