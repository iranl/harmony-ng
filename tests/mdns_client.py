#!/usr/bin/env python3
"""Minimal mDNS/DNS packet helpers for the tests (pure stdlib)."""

import socket
import struct

TYPE_A, TYPE_PTR, TYPE_TXT, TYPE_SRV, TYPE_ANY = 1, 12, 16, 33, 255


def encode_name(name):
    out = b""
    for label in name.strip(".").split("."):
        b = label.encode("utf-8")
        out += bytes([len(b)]) + b
    return out + b"\x00"


def build_query(questions, answers=(), qid=0, flags=0):
    """questions: [(name, type, unicast)], answers: [(name, type, ttl, rdata_bytes)]"""
    pkt = struct.pack("!HHHHHH", qid, flags, len(questions), len(answers), 0, 0)
    for name, qtype, qu in questions:
        pkt += encode_name(name) + struct.pack("!HH", qtype, 0x8001 if qu else 1)
    for name, rtype, ttl, rdata in answers:
        pkt += encode_name(name) + struct.pack("!HHIH", rtype, 1, ttl, len(rdata)) + rdata
    return pkt


def read_name(pkt, pos):
    labels = []
    jumped = False
    end = None
    hops = 0
    while True:
        l = pkt[pos]
        if l & 0xC0 == 0xC0:
            ptr = ((l & 0x3F) << 8) | pkt[pos + 1]
            if not jumped:
                end = pos + 2
            jumped = True
            pos = ptr
            hops += 1
            if hops > 20:
                raise ValueError("pointer loop")
            continue
        pos += 1
        if l == 0:
            break
        labels.append(pkt[pos:pos + l].decode("utf-8", "replace"))
        pos += l
    return ".".join(labels), (end if jumped else pos)


def parse_packet(pkt):
    qid, flags, qd, an, ns, ar = struct.unpack("!HHHHHH", pkt[:12])
    pos = 12
    questions = []
    for _ in range(qd):
        name, pos = read_name(pkt, pos)
        qtype, qcls = struct.unpack("!HH", pkt[pos:pos + 4])
        pos += 4
        questions.append({"name": name, "type": qtype, "unicast": bool(qcls & 0x8000)})
    sections = {"answers": [], "authority": [], "additional": []}
    for sec, count in (("answers", an), ("authority", ns), ("additional", ar)):
        for _ in range(count):
            name, pos = read_name(pkt, pos)
            rtype, rcls, ttl, rdlen = struct.unpack("!HHIH", pkt[pos:pos + 10])
            pos += 10
            rdata = pkt[pos:pos + rdlen]
            rec = {"name": name, "type": rtype, "flush": bool(rcls & 0x8000), "ttl": ttl}
            if rtype == TYPE_PTR:
                rec["target"], _ = read_name(pkt, pos)
            elif rtype == TYPE_SRV:
                pri, wt, port = struct.unpack("!HHH", rdata[:6])
                rec["port"] = port
                rec["target"], _ = read_name(pkt, pos + 6)
            elif rtype == TYPE_TXT:
                items = []
                p = 0
                while p < len(rdata):
                    n = rdata[p]
                    items.append(rdata[p + 1:p + 1 + n].decode("utf-8", "replace"))
                    p += 1 + n
                rec["txt"] = items
            elif rtype == TYPE_A:
                rec["ip"] = ".".join(str(b) for b in rdata[:4])
            pos += rdlen
            sections[sec].append(rec)
    return {"id": qid, "flags": flags, "questions": questions, **sections}


def query(host, port, questions, answers=(), timeout=2.0, qid=0):
    """Sends a query from an ephemeral port (legacy unicast style) and returns the parsed reply
    packets received within the timeout."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(build_query(questions, answers, qid=qid), (host, port))
    replies = []
    try:
        while True:
            data, _ = s.recvfrom(4096)
            replies.append(parse_packet(data))
            s.settimeout(0.3)
    except socket.timeout:
        pass
    s.close()
    return replies
