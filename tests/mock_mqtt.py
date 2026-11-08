#!/usr/bin/env python3
"""
A small MQTT 3.1.1 broker and client for the tests, pure stdlib, so the suite runs without
mosquitto or paho: CONNECT/CONNACK with a will, PUBLISH QoS 0/1 (PUBACK), SUBSCRIBE/SUBACK with
+ and # wildcards, retained messages (an empty payload clears one), PINGREQ/PINGRESP, DISCONNECT,
and the will on a connection loss. Not for production: one thread per client, no persistence.
"""

import socket
import struct
import threading
import time


def _enc_len(n):
    out = b""
    while True:
        d = n % 128
        n //= 128
        if n > 0:
            d |= 0x80
        out += bytes([d])
        if n == 0:
            return out


def _enc_str(s):
    b = s.encode("utf-8") if isinstance(s, str) else s
    return struct.pack("!H", len(b)) + b


def topic_matches(sub, topic):
    s = sub.split("/")
    t = topic.split("/")
    for i, part in enumerate(s):
        if part == "#":
            return True
        if i >= len(t):
            return False
        if part != "+" and part != t[i]:
            return False
    return len(s) == len(t)


def _read_packet(sock):
    """Returns (type_byte, payload bytes) or None on EOF"""
    hdr = sock.recv(1)
    if not hdr:
        return None
    mult = 1
    rem = 0
    while True:
        b = sock.recv(1)
        if not b:
            return None
        rem += (b[0] & 0x7F) * mult
        mult *= 128
        if not (b[0] & 0x80):
            break
    data = b""
    while len(data) < rem:
        chunk = sock.recv(rem - len(data))
        if not chunk:
            return None
        data += chunk
    return hdr[0], data


class MockBroker:
    def __init__(self, host="127.0.0.1", port=0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.lock = threading.Lock()
        self.retained = {}          # topic -> payload bytes
        self.clients = []           # _Client
        self.log = []               # (topic, payload, retain, qos) of every publish received
        self.running = True
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        while self.running:
            try:
                s, _ = self.sock.accept()
            except OSError:
                return
            c = _Client(self, s)
            with self.lock:
                self.clients.append(c)
            threading.Thread(target=c.run, daemon=True).start()

    def deliver(self, topic, payload, retain, qos, origin=None):
        with self.lock:
            self.log.append((topic, payload, retain, qos))
            if retain:
                if payload:
                    self.retained[topic] = payload
                else:
                    self.retained.pop(topic, None)
            targets = list(self.clients)
        for c in targets:
            c.maybe_send(topic, payload, False)

    def remove(self, c):
        with self.lock:
            if c in self.clients:
                self.clients.remove(c)

    def messages(self, topic_filter):
        return [(t, p) for (t, p, r, q) in list(self.log) if topic_matches(topic_filter, t)]

    def clear_log(self):
        with self.lock:
            self.log = []

    def close(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass
        for c in list(self.clients):
            c.close()


class _Client:
    def __init__(self, broker, sock):
        self.broker = broker
        self.sock = sock
        self.subs = []
        self.will = None
        self.connected = False
        self.wlock = threading.Lock()
        self.client_id = ""

    def send(self, data):
        with self.wlock:
            try:
                self.sock.sendall(data)
            except OSError:
                pass

    def publish_to(self, topic, payload, retain_flag):
        self.send(bytes([0x30 | (1 if retain_flag else 0)]) + _enc_len(2 + len(topic) + len(payload)) + _enc_str(topic) + payload)

    def maybe_send(self, topic, payload, retain_flag):
        if not self.connected:
            return
        for sub in self.subs:
            if topic_matches(sub, topic):
                self.publish_to(topic, payload, retain_flag)
                return

    def run(self):
        try:
            while True:
                pkt = _read_packet(self.sock)
                if pkt is None:
                    break
                t, data = pkt
                ptype = t & 0xF0
                if ptype == 0x10:
                    self._connect(data)
                elif ptype == 0x30:
                    self._publish(t, data)
                elif ptype == 0x80:
                    self._subscribe(data)
                elif ptype == 0xA0:
                    pid = data[:2]
                    self.send(b"\xB0\x02" + pid)
                elif ptype == 0xC0:
                    self.send(b"\xD0\x00")
                elif ptype == 0xE0:
                    self.will = None
                    break
                # PUBACK (0x40) from the client: ignored
        except OSError:
            pass
        self.close()

    def _connect(self, data):
        pos = 0
        plen = struct.unpack("!H", data[pos:pos + 2])[0]
        pos += 2 + plen  # protocol name
        pos += 1          # level
        flags = data[pos]
        pos += 1
        pos += 2          # keepalive
        cid_len = struct.unpack("!H", data[pos:pos + 2])[0]
        self.client_id = data[pos + 2:pos + 2 + cid_len].decode("utf-8", "replace")
        pos += 2 + cid_len
        if flags & 0x04:
            wl = struct.unpack("!H", data[pos:pos + 2])[0]
            wtopic = data[pos + 2:pos + 2 + wl].decode("utf-8", "replace")
            pos += 2 + wl
            pl = struct.unpack("!H", data[pos:pos + 2])[0]
            wpayload = data[pos + 2:pos + 2 + pl]
            pos += 2 + pl
            self.will = (wtopic, wpayload, bool(flags & 0x20))
        self.connected = True
        self.send(b"\x20\x02\x00\x00")

    def _publish(self, t, data):
        qos = (t >> 1) & 3
        retain = bool(t & 1)
        tlen = struct.unpack("!H", data[:2])[0]
        topic = data[2:2 + tlen].decode("utf-8", "replace")
        pos = 2 + tlen
        if qos > 0:
            pid = data[pos:pos + 2]
            pos += 2
            self.send(b"\x40\x02" + pid)
        payload = data[pos:]
        self.broker.deliver(topic, payload, retain, qos, origin=self)

    def _subscribe(self, data):
        pid = data[:2]
        pos = 2
        codes = b""
        new = []
        while pos < len(data):
            tl = struct.unpack("!H", data[pos:pos + 2])[0]
            topic = data[pos + 2:pos + 2 + tl].decode("utf-8", "replace")
            pos += 2 + tl
            qos = data[pos]
            pos += 1
            self.subs.append(topic)
            new.append(topic)
            codes += bytes([min(qos, 1)])
        self.send(b"\x90" + _enc_len(2 + len(codes)) + pid + codes)
        with self.broker.lock:
            items = list(self.broker.retained.items())
        for topic, payload in items:
            for sub in new:
                if topic_matches(sub, topic):
                    self.publish_to(topic, payload, True)
                    break

    def close(self):
        was_connected = self.connected
        self.connected = False
        try:
            self.sock.close()
        except OSError:
            pass
        self.broker.remove(self)
        if was_connected and self.will:
            wt, wp, wr = self.will
            self.will = None
            self.broker.deliver(wt, wp, wr, 0)


class MockClient:
    """Minimal MQTT client for the tests: connect, subscribe, publish, receive with a timeout."""

    def __init__(self, host, port, client_id="test-client", timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.inbox = []
        self.lock = threading.Lock()
        self.pid = 1
        payload = _enc_str("MQTT") + b"\x04\x02" + struct.pack("!H", 60) + _enc_str(client_id)
        self.sock.sendall(b"\x10" + _enc_len(len(payload)) + payload)
        t, data = _read_packet(self.sock)
        assert t == 0x20 and data[1] == 0, "CONNACK"
        self.running = True
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        try:
            while self.running:
                pkt = _read_packet(self.sock)
                if pkt is None:
                    break
                t, data = pkt
                if t & 0xF0 == 0x30:
                    tlen = struct.unpack("!H", data[:2])[0]
                    topic = data[2:2 + tlen].decode("utf-8", "replace")
                    pos = 2 + tlen
                    qos = (t >> 1) & 3
                    if qos:
                        pid = data[pos:pos + 2]
                        pos += 2
                        self.sock.sendall(b"\x40\x02" + pid)
                    with self.lock:
                        self.inbox.append((topic, data[pos:], bool(t & 1)))
        except OSError:
            pass

    def subscribe(self, topic):
        pid = struct.pack("!H", self.pid)
        self.pid += 1
        body = pid + _enc_str(topic) + b"\x01"
        self.sock.sendall(b"\x82" + _enc_len(len(body)) + body)

    def publish(self, topic, payload, retain=False, qos=0):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        body = _enc_str(topic)
        if qos:
            body += struct.pack("!H", self.pid)
            self.pid += 1
        body += payload
        self.sock.sendall(bytes([0x30 | (qos << 1) | (1 if retain else 0)]) + _enc_len(len(body)) + body)

    def wait_for(self, topic_filter, timeout=5.0, where=None):
        """First message matching the filter (and where(payload) if given): (topic, payload, retain)"""
        deadline = time.time() + timeout
        seen = set()
        while time.time() < deadline:
            with self.lock:
                for i, m in enumerate(self.inbox):
                    if i in seen:
                        continue
                    seen.add(i)
                    if topic_matches(topic_filter, m[0]) and (where is None or where(m[1])):
                        return m
            time.sleep(0.05)
        return None

    def drain(self):
        with self.lock:
            out = list(self.inbox)
            self.inbox = []
        return out

    def close(self):
        self.running = False
        try:
            self.sock.sendall(b"\xE0\x00")
            self.sock.close()
        except OSError:
            pass


if __name__ == "__main__":
    b = MockBroker()
    a = MockClient("127.0.0.1", b.port, "a")
    c = MockClient("127.0.0.1", b.port, "c")
    a.publish("x/retained", "1", retain=True, qos=1)
    time.sleep(0.2)
    c.subscribe("x/#")
    m = c.wait_for("x/retained", 2)
    assert m and m[1] == b"1" and m[2], m
    a.publish("x/live", "2", qos=1)
    m = c.wait_for("x/live", 2)
    assert m and m[1] == b"2" and not m[2], m
    a.publish("x/retained", "", retain=True)
    time.sleep(0.2)
    assert "x/retained" not in b.retained
    c.close()
    a.close()
    b.close()
    print("mock_mqtt self test ok")
