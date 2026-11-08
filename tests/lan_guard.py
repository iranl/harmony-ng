"""
Helpers for tests/test_no_lan_responders.py (and the SSDP part on milestone 4): which UDP ports a
process has bound, and whether multicast sent to a group reaches a listener joined on loopback,
on the host's LAN interface, or both.
"""

import os
import socket
import struct
import time

IP_MULTICAST_ALL = 49  # Linux: 0 = deliver only groups this socket joined itself


def udp_ports_of(pid):
    """Local UDP ports of the sockets process pid holds (from /proc/<pid>/fd and /proc/net/udp)"""
    inodes = set()
    fd_dir = "/proc/%d/fd" % pid
    for name in os.listdir(fd_dir):
        try:
            target = os.readlink(os.path.join(fd_dir, name))
        except OSError:
            continue
        if target.startswith("socket:["):
            inodes.add(target[8:-1])
    ports = set()
    with open("/proc/net/udp") as f:
        next(f)
        for line in f:
            cols = line.split()
            if len(cols) > 9 and cols[9] in inodes:
                ports.add(int(cols[1].split(":")[1], 16))
    return ports


def lan_address():
    """The address of the interface the default route uses, or None (a host with loopback only)"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))  # TEST-NET-1, nothing is sent
        ip = s.getsockname()[0]
    except OSError:
        ip = None
    finally:
        s.close()
    return ip if ip and not ip.startswith("127.") else None


def group_listener(group, port, iface_ip):
    """A socket that receives group:port only as joined on iface_ip"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.IPPROTO_IP, IP_MULTICAST_ALL, 0)
    s.bind(("", port))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group) + socket.inet_aton(iface_ip))
    return s


def collect(sock, seconds, want=None):
    """Datagrams received within seconds: [(data, sender)]; want(data) filters"""
    out = []
    deadline = time.monotonic() + seconds
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            break
        sock.settimeout(left)
        try:
            data, addr = sock.recvfrom(4096)
        except socket.timeout:
            break
        if want is None or want(data):
            out.append((data, addr))
    return out


def send_local_multicast(group, port, iface_ip, payload=b"lan-guard-control"):
    """Control packet: multicast via iface_ip with TTL 0 and loop on, so it is delivered to local
    listeners only and never put on the wire"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface_ip))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    s.sendto(payload, (group, port))
    s.close()
    return payload
