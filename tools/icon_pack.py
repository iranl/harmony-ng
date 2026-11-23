#!/usr/bin/env python3
"""Repack the remote icon archive (icons.tar.gz) so the Hub can read single icons from flash.

The result is still one ordinary gzip member holding a tar: tar -xzf, the web UIs and older Hub
firmware read it as before. Inside, the deflate stream is cut by a full flush every SEGMENT_RAW
bytes of tar data (always between two tar entries), so each part can be inflated on its own. The
last tar entry, icons/.pack-index, is a text index, and a gzip FEXTRA subfield 'HI' tells where its
part starts. hub_daemon and hub_rf (payload/source/icon_pack.c) inflate one part per icon instead
of unpacking about 6 MB of icons into /tmp, which is RAM.

Index lines:
    HIP1
    S <file offset> <compressed length> <tar bytes> <CRC32 of the tar bytes>   one per part, in order
    F <part> <offset in part> <size> <name>              sorted by name (byte order)
<name> is the path below icons/ ("hub/wifi.png"), and every PNG of a folder one level down is also
listed by its file name alone ("wifi.png"): the old /tmp unpacking linked them so, a later folder
(name order) replacing an earlier one and both replacing a file of that name in icons/.

Usage: python tools/icon_pack.py ARCHIVE [OUT]   (no OUT: ARCHIVE is replaced)
"""

from __future__ import annotations

import os
import pathlib
import struct
import sys
import tarfile
import tempfile
import zlib

SEGMENT_RAW = 64 * 1024
INDEX_NAME = "icons/.pack-index"
EXTRA_ID = b"HI"
EXTRA_VERSION = 1
# gzip header: 10 bytes, XLEN, then one subfield: SI1 SI2 LEN(2) + 20 bytes of data
EXTRA_DATA_LEN = 20
HEADER_LEN = 10 + 2 + 4 + EXTRA_DATA_LEN


def is_packed(path: pathlib.Path) -> bool:
    """True when path already has the pack index of this version."""
    try:
        with open(path, "rb") as f:
            head = f.read(HEADER_LEN)
    except OSError:
        return False
    return (len(head) == HEADER_LEN and head[:4] == b"\x1f\x8b\x08\x04"
            and head[12:14] == EXTRA_ID and head[16] == EXTRA_VERSION)


def _entry_bytes(name: str, data: bytes, mtime: int) -> bytes:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mtime = mtime
    info.mode = 0o644
    return info.tobuf(tarfile.USTAR_FORMAT) + data + b"\0" * (-len(data) % 512)


def pack(src: pathlib.Path, dst: pathlib.Path) -> None:
    """Write the packed form of the icon archive src to dst (src and dst may be the same file)."""
    files = {}
    with tarfile.open(src, "r:*") as tf:
        for m in tf:
            if m.isfile() and m.name != INDEX_NAME:
                files[m.name] = (tf.extractfile(m).read(), int(m.mtime))
    if not files:
        raise ValueError(f"{src}: no files in the archive")

    comp = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    body = bytearray()
    crc = 0
    total = 0
    segments = []  # [file offset, compressed length, tar bytes, CRC32 of the tar bytes]
    seg_raw = 0
    seg_crc = 0

    def feed(raw: bytes) -> None:
        nonlocal crc, total, seg_raw, seg_crc
        body.extend(comp.compress(raw))
        crc = zlib.crc32(raw, crc)
        seg_crc = zlib.crc32(raw, seg_crc)
        total += len(raw)
        seg_raw += len(raw)

    def new_segment() -> None:
        nonlocal seg_raw, seg_crc
        if segments:
            body.extend(comp.flush(zlib.Z_FULL_FLUSH))
            segments[-1][1] = HEADER_LEN + len(body) - segments[-1][0]
            segments[-1][2] = seg_raw
            segments[-1][3] = seg_crc & 0xFFFFFFFF
        segments.append([HEADER_LEN + len(body), 0, 0, 0])
        seg_raw = 0
        seg_crc = 0

    where = {}  # name below icons/ -> (part, offset in part, size)
    for name in sorted(files, key=lambda n: n.encode()):
        data, mtime = files[name]
        entry = _entry_bytes(name, data, mtime)
        # a part ends before the file that would take it past SEGMENT_RAW: one read inflates at
        # most about SEGMENT_RAW plus one file (it was SEGMENT_RAW plus the next file)
        if not segments or (seg_raw > 0 and seg_raw + len(entry) > SEGMENT_RAW):
            new_segment()
        if name.startswith("icons/"):
            where[name[6:]] = (len(segments) - 1, seg_raw + 512, len(data))
        feed(entry)

    names = dict(where)
    folders = sorted({n.split("/")[0] for n in where if n.count("/") == 1}, key=lambda n: n.encode())
    for folder in folders:
        for n, loc in where.items():
            if n.startswith(folder + "/") and n.count("/") == 1 and n.endswith(".png"):
                names[n.split("/")[1]] = loc

    new_segment()
    index_off = segments[-1][0]
    segments.pop()
    lines = ["HIP1"] + [f"S {o} {c} {r} {k}" for o, c, r, k in segments]
    lines += [f"F {s} {o} {n} {name}" for name, (s, o, n) in sorted(names.items(), key=lambda kv: kv[0].encode())]
    index = ("\n".join(lines) + "\n").encode()
    feed(_entry_bytes(INDEX_NAME, index, 0) + b"\0" * 1024)  # two zero blocks end the tar
    body.extend(comp.flush())
    index_comp_len = HEADER_LEN + len(body) - index_off
    index_raw_len = seg_raw

    extra = struct.pack("<B3xIIII", EXTRA_VERSION, index_off, index_comp_len, index_raw_len, len(index))
    header = b"\x1f\x8b\x08\x04" + b"\0\0\0\0" + b"\x02\xff"
    header += struct.pack("<H", 4 + len(extra)) + EXTRA_ID + struct.pack("<H", len(extra)) + extra
    assert len(header) == HEADER_LEN
    out = header + bytes(body) + struct.pack("<II", crc & 0xFFFFFFFF, total & 0xFFFFFFFF)

    dst = pathlib.Path(dst)
    fd, tmp = tempfile.mkstemp(prefix=".icon_pack.", dir=dst.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(out)
        os.replace(tmp, dst)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3) or argv[1].startswith("-"):
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    src = pathlib.Path(argv[1])
    dst = pathlib.Path(argv[2]) if len(argv) == 3 else src
    pack(src, dst)
    print(f"[+] {dst}: packed ({dst.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
