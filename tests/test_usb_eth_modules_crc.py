#!/usr/bin/env python3
"""Every module in payload/modules must load on the stock Hub kernel (modversions).

The kernel refuses a module that records no CRC for a symbol it uses ("no symbol version for
usb_clear_halt", "Unknown symbol ..."): usbnet.ko, smsc95xx.ko and others were shipped that way.
The test reads each .ko (big endian ELF32) and checks that __versions names every undefined
symbol. build/build_usb_ethernet.sh makes the same check before it copies a module.

A CRC per symbol does not prove the struct layout: modules built with CONFIG_XFRM and
CONFIG_IPV6_NDISC_NODETYPE loaded but read struct sk_buff 4 and 8 bytes off. So the test also
checks, in each module that calls eth_type_trans(), that the result goes to skb->protocol at the
stock offset 104 (sh v0 right after the call), and, after each skb allocation, that the
inlined skb_reserve() writes skb->data and skb->tail at the stock offsets 152 and 140 (the
first two `sw` with an offset of 100 or more). The two anchors sit on both sides of the places
where sk_buff can grow.
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


STOCK_SKB_PROTOCOL = 104
STOCK_SKB_RESERVE = [140, 152]  # tail, data
SKB_ALLOCS = ("__alloc_skb", "dev_alloc_skb", "__netdev_alloc_skb")


def sections(b: bytes) -> list[tuple]:
    shoff, = struct.unpack(">I", b[32:36])
    shentsize, shnum = struct.unpack(">HH", b[46:50])
    return [struct.unpack(">IIIIIIIIII", b[shoff + i * shentsize: shoff + i * shentsize + 40]) for i in range(shnum)]


def symbol_name(b: bytes, secs: list[tuple], symtab: tuple, index: int) -> str:
    name, = struct.unpack(">I", b[symtab[4] + index * 16: symtab[4] + index * 16 + 4])
    start = secs[symtab[6]][4] + name
    return b[start:b.index(b"\x00", start)].decode()


def stores_after(ko: pathlib.Path, callees: tuple[str, ...], opcode: int, rt: int | None,
                 window: int, count: int, min_off: int = 0) -> list[list[int]]:
    """For each call of one of `callees`: the offsets of the first `count` stores (`opcode`,
    to register `rt` if given, offset >= min_off) in the next `window` instructions."""
    b = ko.read_bytes()
    secs = sections(b)
    out = []
    for s in secs:
        if s[1] != 9:  # SHT_REL
            continue
        text = secs[s[7]]  # sh_info: the section the relocations apply to
        for o in range(s[4], s[4] + s[5], 8):
            r_offset, r_info = struct.unpack(">II", b[o:o + 8])
            # modules are built with -mlong-calls: lui/addiu (R_MIPS_HI16/LO16), then jalr
            if r_info & 0xFF != 6 or symbol_name(b, secs, secs[s[6]], r_info >> 8) not in callees:
                continue
            found = []
            for i in range(1, window + 1):
                insn, = struct.unpack(">I", b[text[4] + r_offset + 4 * i: text[4] + r_offset + 4 * i + 4])
                off = struct.unpack(">h", struct.pack(">H", insn & 0xFFFF))[0]
                if insn >> 26 == opcode and (rt is None or (insn >> 16) & 0x1F == rt) and off >= min_off:
                    found.append(off)
                    if len(found) == count:
                        break
            if found:
                out.append(found)
    return out


def protocol_stores(ko: pathlib.Path) -> list[int]:
    """Offsets of the first `sh v0,off(reg)` after each call of eth_type_trans()."""
    return [f[0] for f in stores_after(ko, ("eth_type_trans",), 0x29, 2, 9, 1)]


def reserve_stores(ko: pathlib.Path) -> list[list[int]]:
    """Sorted offsets of the first two `sw ...,off(reg)` (off >= 100) after an skb allocation."""
    return [sorted(f) for f in stores_after(ko, SKB_ALLOCS, 0x2B, None, 30, 2, 100) if len(f) == 2]


def undefined_symbols(ko: pathlib.Path) -> set[str]:
    b = ko.read_bytes()
    secs = sections(b)
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

    def test_sk_buff_has_the_stock_layout(self):
        checked = 0
        for ko in sorted(MODULES.glob("*.ko")):
            stores = protocol_stores(ko)
            with self.subTest(module=ko.name):
                self.assertEqual([o for o in stores if o != STOCK_SKB_PROTOCOL], [],
                                 "skb->protocol written at another offset than the stock kernel's")
            checked += bool(stores)
        self.assertGreaterEqual(checked, 4)  # usbnet, pegasus, rtl8150, r8152

    def test_skb_reserve_has_the_stock_layout(self):
        checked = 0
        for ko in sorted(MODULES.glob("*.ko")):
            pairs = reserve_stores(ko)
            with self.subTest(module=ko.name):
                self.assertEqual([p for p in pairs if p != STOCK_SKB_RESERVE], [],
                                 "skb->tail/data written at other offsets than the stock kernel's")
            checked += bool(pairs)
        self.assertGreaterEqual(checked, 3)  # usbnet, pegasus, rtl8150


if __name__ == "__main__":
    unittest.main()
