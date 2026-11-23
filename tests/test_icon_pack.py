#!/usr/bin/env python3
"""tools/icon_pack.py: the packed archive is still a plain tar.gz with the same files, every index
entry points at its file's bytes inside a part that inflates on its own, folder PNGs are listed by
their file name the way the old /tmp unpacking linked them, and packing is reproducible."""

import gzip
import io
import os
import pathlib
import sys
import tarfile
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import icon_pack  # noqa: E402


def make_archive(path, files):
    with tarfile.open(path, "w:gz") as tf:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = 1700000000
            tf.addfile(info, io.BytesIO(data))


def read_index(blob):
    ver, off, clen, rlen, isize = __import__("struct").unpack("<B3xIIII", blob[16:36])
    raw = zlib.decompressobj(-15).decompress(blob[off:off + clen])
    assert len(raw) == rlen, (len(raw), rlen)
    text = raw[512:512 + isize].decode()
    lines = text.splitlines()
    assert lines[0] == "HIP1"
    segs = [tuple(map(int, l.split()[1:])) for l in lines if l.startswith("S ")]
    ents = {}
    for l in lines:
        if l.startswith("F "):
            _, s, o, n, name = l.split(" ", 4)
            ents[name] = (int(s), int(o), int(n))
    return segs, ents, [l.split(" ", 4)[4] for l in lines if l.startswith("F ")]


def segment_data(blob, seg):
    off, clen, rlen = seg[:3]
    raw = zlib.decompressobj(-15).decompress(blob[off:off + clen] + b"\x03\x00")
    assert len(raw) == rlen, (len(raw), rlen)
    if len(seg) > 3:  # the CRC32 of the part's tar bytes (the reader checks it)
        assert zlib.crc32(raw) == seg[3], ("crc", zlib.crc32(raw), seg[3])
    return raw


def make_files():
    rnd = __import__("random").Random(7)
    files = {}
    for i in range(60):
        files[f"icons/top{i:02d}.png"] = bytes(rnd.getrandbits(8) for _ in range(rnd.randint(100, 9000)))
    files["icons/big.png"] = bytes(rnd.getrandbits(8) for _ in range(90000))  # larger than a part
    files["icons/a/shared.png"] = b"from-a"
    files["icons/b/shared.png"] = b"from-b"   # later folder wins
    files["icons/b/top01.png"] = b"folder-wins"  # a folder PNG replaces the file in icons/
    files["icons/a/note.txt"] = b"not linked"
    files["icons/a/deep/x.png"] = b"two levels: not linked"
    return files


def write_fixture(out):
    """Archives for tests/test_icon_pack.c: icons.tar.gz (packed), plain.tar.gz, big.png"""
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    files = make_files()
    make_archive(out / "plain.tar.gz", files)
    icon_pack.pack(out / "plain.tar.gz", out / "icons.tar.gz")
    (out / "big.png").write_bytes(files["icons/big.png"])


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--fixture":
        write_fixture(sys.argv[2])
        return
    files = make_files()

    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "icons.tar.gz"
        make_archive(src, files)
        assert not icon_pack.is_packed(src)
        icon_pack.pack(src, src)
        assert icon_pack.is_packed(src)
        blob = src.read_bytes()

        # still a plain tar.gz with the same files (plus the index)
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(blob))) as tf:
            got = {m.name: tf.extractfile(m).read() for m in tf if m.isfile()}
        assert got.pop(icon_pack.INDEX_NAME)
        assert got == files

        segs, ents, order = read_index(blob)
        assert len(segs) > 3
        assert order == sorted(order, key=lambda n: n.encode())
        cache = {}
        for name, (s, o, n) in ents.items():
            raw = cache.setdefault(s, segment_data(blob, segs[s]))
            data = raw[o:o + n]
            if "/" in name:
                assert data == files["icons/" + name], name
        flat = {k: v for k, v in ents.items() if "/" not in k}
        def data_of(name):
            s, o, n = ents[name]
            return segment_data(blob, segs[s])[o:o + n]
        assert data_of("shared.png") == b"from-b"
        assert data_of("top01.png") == b"folder-wins"
        assert data_of("top02.png") == files["icons/top02.png"]
        assert "note.txt" not in flat and "x.png" not in flat
        assert data_of("a/deep/x.png") == b"two levels: not linked"

        # packing a packed archive again gives the same bytes (deploy compares MD5)
        again = pathlib.Path(tmp) / "again.tar.gz"
        icon_pack.pack(src, again)
        assert again.read_bytes() == blob
    print("test_icon_pack: OK")


if __name__ == "__main__":
    main()
