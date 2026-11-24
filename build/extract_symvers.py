#!/usr/bin/env python3
"""Write a Module.symvers for the stock Hub kernel (2.6.31-g89d565c, MIPS big endian) from its
raw image, so kernel modules can be built against it without Logitech's build tree.

The kernel was built with CONFIG_MODVERSIONS: a module loads only when the CRC it recorded for
each kernel symbol matches the kernel's own. The raw image (vmlinux.bin, load address
0x80002000, e.g. decompressed from mtd1) holds the export tables: __ksymtab and __ksymtab_gpl
(value, name pointer pairs, one run) followed by __kcrctab and __kcrctab_gpl (one CRC per entry,
same order).

  python3 build/extract_symvers.py <vmlinux.bin> <Module.symvers> [--check payload/modules]

--check compares the CRCs with the __versions sections of modules that load on the Hub and
fails on any difference.
"""

import argparse
import pathlib
import struct
import sys

BASE = 0x80002000


def exports(data: bytes, calibrate: dict[str, int] | None = None) -> dict[str, int]:
    end = BASE + len(data)

    def u32(off):
        return struct.unpack(">I", data[off:off + 4])[0]

    def entry_name(off):
        value, name = u32(off), u32(off + 4)
        if not (BASE <= name < end and BASE <= value < 0xC0000000):
            return None
        s = name - BASE
        if s == 0 or data[s - 1] != 0:
            return None
        e = data.find(b"\x00", s, s + 128)
        if e <= s or not all(32 < c < 127 for c in data[s:e]):
            return None
        return data[s:e].decode()

    # the export table is the longest run of valid (value, name) pairs
    best = (0, 0)
    off = 0
    while off < len(data) - 8:
        if entry_name(off) is None:
            off += 4
            continue
        start = off
        while entry_name(off) is not None:
            off += 8
        if off - start > best[1] - best[0]:
            best = (start, off)
    start, stop = best
    count = (stop - start) // 8
    if count < 500:
        sys.exit("no export table found (%d entries)" % count)
    names = [entry_name(start + i * 8) for i in range(count)]
    # The CRC table follows after a small gap (one word on the stock image): the gap is the
    # one where the CRCs of known modules match (calibrate), else the first word after the run
    gaps = range(0, 64, 4)
    if calibrate:
        scores = []
        for gap in gaps:
            crcs = {n: u32(stop + gap + i * 4) for i, n in enumerate(names)}
            scores.append((sum(crcs.get(n) == c for n, c in calibrate.items()), gap))
        hits, gap = max(scores)
    else:
        gap = 4
    return {n: u32(stop + gap + i * 4) for i, n in enumerate(names)}


def module_versions(ko: pathlib.Path) -> dict[str, int]:
    """CRCs a module recorded (__versions: 4-byte CRC + 60-byte name), big endian ELF32."""
    b = ko.read_bytes()
    shoff, = struct.unpack(">I", b[32:36])
    shentsize, shnum, shstrndx = struct.unpack(">HHH", b[46:52])
    secs = [struct.unpack(">IIIIIIIIII", b[shoff + i * shentsize: shoff + i * shentsize + 40]) for i in range(shnum)]
    strtab = secs[shstrndx]
    out = {}
    for s in secs:
        n = b[strtab[4] + s[0]: b.index(b"\x00", strtab[4] + s[0])]
        if n == b"__versions":
            for o in range(s[4], s[4] + s[5], 64):
                crc, = struct.unpack(">I", b[o:o + 4])
                out[b[o + 4:o + 64].split(b"\x00")[0].decode()] = crc
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vmlinux")
    ap.add_argument("symvers")
    ap.add_argument("--check", help="directory of modules that load on the Hub")
    a = ap.parse_args()
    known = {}
    if a.check:
        for ko in sorted(pathlib.Path(a.check).glob("*.ko")):
            known.update(module_versions(ko))
    syms = exports(pathlib.Path(a.vmlinux).read_bytes(), known)
    with open(a.symvers, "w", newline="\n") as f:
        for name in sorted(syms):
            f.write("0x%08x\t%s\tvmlinux\tEXPORT_SYMBOL\n" % (syms[name], name))
    print("%d kernel exports written to %s" % (len(syms), a.symvers))
    if a.check:
        bad = checked = 0
        for ko in sorted(pathlib.Path(a.check).glob("*.ko")):
            for name, crc in module_versions(ko).items():
                if name in syms:
                    checked += 1
                    if syms[name] != crc:
                        bad += 1
                        print("MISMATCH %s: %s 0x%08x, kernel 0x%08x" % (ko.name, name, crc, syms[name]))
        print("%d module CRCs checked, %d mismatches" % (checked, bad))
        if bad or not checked:
            sys.exit(1)


if __name__ == "__main__":
    main()
