#!/usr/bin/env python3
"""Archives for tests/test_icon_pack_verify.c, written into OUT:
a.tar.gz, b.tar.gz   packs of two icon sets with the same names and sizes, other bytes
a/, b/               the icons of each set (name below icons/, no folders)
aflip.tar.gz         a.tar.gz with one byte of big.png's part changed: same size and index,
                     the part still inflates to its length but to other bytes
badcrc.tar.gz        set a packed, every part CRC in the index wrong by one bit
cut.tar.gz, cut2.tar.gz   the first half / third of a.tar.gz (a copy cut short)
vnext.tar.gz         a.tar.gz with the next pack version in its header (a pack this firmware does not read)
hi_other.tar.gz      a gzip file whose first subfield is 'HI' but not the pack layout (XLEN 26)
plain.tar.gz         set a, not packed
Usage: icon_pack_verify_fixture.py OUT"""

import gzip
import pathlib
import random
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import icon_pack  # noqa: E402
from test_icon_pack import make_archive  # noqa: E402


def icon_set(seed):
    """Random icons of the same names and sizes in both sets, bytes from seed"""
    sizes = random.Random(7)
    rnd = random.Random(seed)
    files = {}
    for i in range(40):
        files[f"icons/top{i:02d}.png"] = bytes(rnd.getrandbits(8) for _ in range(sizes.randint(100, 9000)))
    files["icons/big.png"] = bytes(rnd.getrandbits(8) for _ in range(sizes.randint(80000, 95000)))
    return files


def wrong_crcs(blob):
    """The pack with each S line's CRC off by one bit (index part recompressed, header updated)"""
    ver, off, clen, rlen, isize = struct.unpack("<B3xIIII", blob[16:36])
    raw = zlib.decompressobj(-15).decompress(blob[off:off + clen])
    text = raw[512:512 + isize].decode()
    lines = []
    for line in text.splitlines():
        if line.startswith("S "):
            f = line.split()
            f[4] = str(int(f[4]) ^ 1)
            line = " ".join(f)
        lines.append(line)
    index = ("\n".join(lines) + "\n").encode()
    entry = icon_pack._entry_bytes(icon_pack.INDEX_NAME, index, 0) + b"\0" * 1024
    comp = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    part = comp.compress(entry) + comp.flush()
    extra = struct.pack("<B3xIIII", ver, off, len(part), len(entry), len(index))
    return blob[:16] + extra + blob[36:off] + part + struct.pack("<II", 0, 0)


def flip_in_part(blob):
    """blob with one byte changed inside the part that holds big.png, chosen so the part still
    inflates to its full length (only its CRC tells the bytes are wrong)"""
    ver, off, clen, rlen, isize = struct.unpack("<B3xIIII", blob[16:36])
    raw = zlib.decompressobj(-15).decompress(blob[off:off + clen])
    lines = raw[512:512 + isize].decode().splitlines()
    parts = [list(map(int, l.split()[1:])) for l in lines if l.startswith("S ")]
    part = next(int(l.split()[1]) for l in lines if l.startswith("F ") and l.split(" ", 4)[4] == "big.png")
    p_off, p_clen, p_raw = parts[part][:3]
    good = zlib.decompressobj(-15).decompress(blob[p_off:p_off + p_clen] + b"\x03\x00")
    for pos in range(p_off + p_clen // 2, p_off + p_clen):
        b = bytearray(blob)
        b[pos] ^= 0x10
        try:
            got = zlib.decompressobj(-15).decompress(bytes(b[p_off:p_off + p_clen]) + b"\x03\x00")
        except zlib.error:
            continue
        if len(got) == p_raw and got != good:
            return bytes(b)
    raise SystemExit("no byte found that keeps the part's length")


def main():
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for tag, seed in (("a", 7), ("b", 11)):
        files = icon_set(seed)
        (out / tag).mkdir(exist_ok=True)
        for name, data in files.items():
            (out / tag / name[6:]).write_bytes(data)
        make_archive(out / f"{tag}.plain.tar.gz", files)
        icon_pack.pack(out / f"{tag}.plain.tar.gz", out / f"{tag}.tar.gz")
    a = (out / "a.tar.gz").read_bytes()
    (out / "aflip.tar.gz").write_bytes(flip_in_part(a))
    (out / "plain.tar.gz").write_bytes((out / "a.plain.tar.gz").read_bytes())
    (out / "badcrc.tar.gz").write_bytes(wrong_crcs(a))
    (out / "cut.tar.gz").write_bytes(a[:len(a) // 2])
    (out / "cut2.tar.gz").write_bytes(a[:len(a) // 3])
    vnext = bytearray(a)
    vnext[16] = icon_pack.EXTRA_VERSION + 1
    (out / "vnext.tar.gz").write_bytes(bytes(vnext))
    # gzip of another tool: FEXTRA, XLEN 26, subfield 'HI' with 22 bytes, then plain set a
    tar = gzip.decompress((out / "a.plain.tar.gz").read_bytes())
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    hi = (b"\x1f\x8b\x08\x04" + bytes(6) + struct.pack("<H", 26) + b"HI" + struct.pack("<H", 22) + bytes(22)
          + co.compress(tar) + co.flush() + struct.pack("<II", zlib.crc32(tar), len(tar) & 0xFFFFFFFF))
    (out / "hi_other.tar.gz").write_bytes(hi)


if __name__ == "__main__":
    main()
