#!/usr/bin/env python3
"""
HTTP and TCP devices (payload/source/net_device.h, net_action.c, tls_client.c):

- device-save keeps the HTTP/TCP fields, refuses bad hosts, ports and headers (400), drops the
  IR fields (controlPort, port, repeats) and sets the HTTP:/TCP: keyCodes;
- GET /api/devices never returns httpPassword / httpToken (hasHttpPassword instead), and a save
  without the password keeps the stored one;
- POST /api/device-net-test runs the command against local servers: method, path with
  {device}/{command}, headers, Basic auth, body; TCP text escapes and hex bytes;
- TLS: pin mode pins the first key, refuses a changed key until the pin is forgotten; verify
  mode refuses a self-signed certificate; insecure mode accepts it;
- /api/ir-send in dry run logs the request instead of sending it;
- the Hub itself is never a device: the test daemon keeps the loopback refusal and allows only
  the ports of this test's servers (HARMONY_TEST_NET_LOOPBACK_PORTS), so a device at 2130706433
  (127.0.0.1) on another port is refused and "localhost" is not saved.
"""

import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 8975
REQUESTS = []


class Recorder(BaseHTTPRequestHandler):
    def _any(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        REQUESTS.append({"method": self.command, "path": self.path, "headers": dict(self.headers), "body": body})
        out = b'{"done":true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    do_GET = do_POST = do_PUT = do_DELETE = _any

    def log_message(self, *a):
        pass


def http_server(tls_cert=None):
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Recorder)
    if tls_cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*tls_cert)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def tls_server_on(port, cert):
    """An HTTPS recorder on a given port (a new key on the same host:port)"""
    srv = ThreadingHTTPServer(("127.0.0.1", port), Recorder)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(*cert)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


class TcpRecorder:
    def __init__(self):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.got = []
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            c.settimeout(2)
            data = b""
            try:
                data = c.recv(4096)
                c.sendall(b"OK\r\n")
            except OSError:
                pass
            self.got.append(data)
            c.close()


def make_cert(d, name):
    key, crt = os.path.join(d, name + ".key"), os.path.join(d, name + ".crt")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt,
                    "-days", "2", "-subj", "/CN=127.0.0.1"], check=True, capture_output=True)
    return crt, key


def config_dir():
    d = tempfile.mkdtemp(prefix="net_dev_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    for name in ("activities.json", "settings.json"):
        shutil.copy(os.path.join(SAMPLE_CONFIG, name), os.path.join(d, name))
    return d


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + str(detail)))
        if not cond:
            failures.append(name)

    tmp = tempfile.mkdtemp(prefix="net_dev_tls_")
    cfg = config_dir()
    log = os.path.join(tmp, "hub.log")
    web = http_server()
    tcp = TcpRecorder()
    cert_a, cert_b = make_cert(tmp, "a"), make_cert(tmp, "b")
    tls = http_server(cert_a)
    tls_port = tls.server_address[1]
    other = http_server()   # a server on 127.0.0.1 whose port the daemon is not told
    ports = ",".join(str(p) for p in (web.server_address[1], tcp.port, tls_port, tls_port + 1, 1))
    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log, env={"HARMONY_TEST_NET_LOOPBACK_PORTS": ports}) as hub:
            def dev_by_id(dev_id):
                st, r = hub.http_get("/api/devices")
                return next((d for d in r.get("devices", []) if d.get("id") == dev_id), None)

            base = {"id": "50", "name": "Kitchen Light", "type": "HTTP", "netHost": "127.0.0.1",
                    "netPort": web.server_address[1], "httpHeaders": [{"name": "X-Test", "value": "abc"}],
                    "httpAuthType": "basic", "httpUser": "u", "httpPassword": "p",
                    "commands": [{"name": "Power", "httpMethod": "POST", "httpPath": "/api/{device}/{command}?x=1",
                                  "httpBody": '{"on":true}'},
                                 {"name": "Status", "httpPath": "/status"}]}
            st, r = hub.http_post("/api/device-save", base)
            check(st == 200 and r.get("ok") is not False, "HTTP device saved", (st, r))
            d = dev_by_id("50")
            check(d is not None and d.get("type") == "HTTP" and "controlPort" not in d, "type HTTP, no controlPort", d)
            cmds = {c["name"]: c for c in (d or {}).get("commands", [])}
            check(cmds.get("Power", {}).get("keyCode") == "HTTP:Power" and "port" not in cmds.get("Power", {})
                  and "repeats" not in cmds.get("Power", {}), "HTTP:<name> keyCode, no IR port/repeats", cmds.get("Power"))
            check(cmds.get("Status", {}).get("httpMethod") == "GET", "method defaults to GET", cmds.get("Status"))
            check(d and d.get("httpPassword") == "" and d.get("hasHttpPassword") is True,
                  "password not returned, hasHttpPassword set", d)

            # saved again without the password: the stored one stays
            again = dict(base)
            again.pop("httpPassword")
            again.pop("commands")
            hub.http_post("/api/device-save", again)

            for bad, what in (({"netPort": 70000}, "port out of range"), ({"netHost": "a b"}, "host with a space"),
                              ({"httpHeaders": [{"name": "Bad Name", "value": "x"}]}, "header name with a space"),
                              ({"httpHeaders": [{"name": "X-A", "value": "a\r\nInjected: 1"}]}, "header value with CR LF"),
                              ({"netTlsVerify": "maybe"}, "unknown verify mode")):
                body = {"id": "50", "type": "HTTP"}
                body.update(bad)
                st, _ = hub.http_post("/api/device-save", body)
                check(st == 400, "400 for " + what, st)

            REQUESTS.clear()
            st, r = hub.http_post("/api/device-net-test", {"id": "50", "command": "Power"}, timeout=15)
            check(st == 200 and r.get("ok") is True and r.get("status") == 200, "net test: HTTP 200", (st, r))
            check('"done"' in r.get("response", ""), "response head returned", r)
            req = REQUESTS[-1] if REQUESTS else {}
            h = req.get("headers", {})
            check(req.get("method") == "POST" and req.get("path") == "/api/Kitchen%20Light/Power?x=1",
                  "method and path with placeholders", req.get("path"))
            check(h.get("Authorization") == "Basic dTpw", "Basic auth from the stored password", h.get("Authorization"))
            check(h.get("X-Test") == "abc" and h.get("Content-Type") == "application/json", "headers", h)
            check(req.get("body") == b'{"on":true}', "body", req.get("body"))

            st, r = hub.http_post("/api/device-net-test", {"id": "50", "command": "Nope"})
            check(st == 404, "404 for an unknown command", st)

            # dry run: /api/ir-send logs the request
            st, r = hub.http_post("/api/ir-send", {"deviceId": "50", "command": "Power"}, timeout=5)
            check(st == 200, "ir-send to the HTTP device", (st, r))
            time.sleep(0.3)
            text = open(log, errors="replace").read()
            check("[DRY-RUN] HTTP POST http://127.0.0.1:%d/api/Kitchen%%20Light/Power?x=1" % web.server_address[1] in text,
                  "dry run logs the request")

            # TCP
            tcp_dev = {"id": "51", "name": "Amp", "type": "TCP", "netHost": "127.0.0.1", "netPort": tcp.port,
                       "commands": [{"name": "On", "tcpPayload": "PWR ON\\r\\x01"},
                                    {"name": "Hex", "tcpFormat": "hex", "tcpPayload": "01 02 ff"},
                                    {"name": "BadHex", "tcpFormat": "hex", "tcpPayload": "0g"}]}
            st, r = hub.http_post("/api/device-save", tcp_dev)
            d = dev_by_id("51")
            cmds = {c["name"]: c for c in (d or {}).get("commands", [])}
            check(d and d.get("type") == "TCP" and cmds.get("On", {}).get("keyCode") == "TCP:On", "TCP device saved", d)
            check(cmds.get("BadHex", {}).get("tcpPayload") == "", "invalid hex payload dropped", cmds.get("BadHex"))
            st, r = hub.http_post("/api/device-net-test", {"id": "51", "command": "On"}, timeout=15)
            check(st == 200 and r.get("ok") is True and "OK" in r.get("response", ""), "TCP test: sent, reply read", r)
            check(tcp.got and tcp.got[-1] == b"PWR ON\r\x01", "text escapes decoded", tcp.got[-1:] )
            hub.http_post("/api/device-net-test", {"id": "51", "command": "Hex"}, timeout=15)
            check(tcp.got and tcp.got[-1] == b"\x01\x02\xff", "hex bytes", tcp.got[-1:])

            # TLS pin mode
            pin_dev = {"id": "52", "name": "Box", "type": "HTTP", "netHost": "127.0.0.1", "netPort": tls_port,
                       "netTls": True, "netTlsVerify": "pin", "httpAuthType": "bearer", "httpToken": "t0k",
                       "commands": [{"name": "Ping", "httpPath": "/ping"}]}
            hub.http_post("/api/device-save", pin_dev)
            d = dev_by_id("52")
            check(d and d.get("httpToken") == "" and d.get("hasHttpToken") is True, "token not returned", d)
            REQUESTS.clear()
            st, r = hub.http_post("/api/device-net-test", {"id": "52", "command": "Ping"}, timeout=15)
            check(r.get("ok") is True and r.get("pinned") is True and len(r.get("tlsFingerprint", "")) == 64,
                  "pin mode: first key pinned", r)
            check(REQUESTS and REQUESTS[-1]["headers"].get("Authorization") == "Bearer t0k", "bearer token sent")
            st, r = hub.http_post("/api/device-net-test", {"id": "52", "command": "Ping"}, timeout=15)
            check(r.get("ok") is True and not r.get("pinned"), "pin mode: same key accepted", r)
            tls.shutdown()
            tls.server_close()
            tls = tls_server_on(tls_port, cert_b)
            st, r = hub.http_post("/api/device-net-test", {"id": "52", "command": "Ping"}, timeout=15)
            check(r.get("ok") is False and "pinned" in r.get("error", ""), "pin mode: changed key refused", r)
            st, r = hub.http_post("/api/device-net-pin-forget", {"id": "52"})
            check(st == 200 and r.get("removed") is True, "pin forgotten", (st, r))
            st, r = hub.http_post("/api/device-net-test", {"id": "52"}, timeout=15)
            check(r.get("ok") is True and r.get("pinned") is True, "connection test pins the new key", r)

            # verify mode: a self-signed certificate is refused; insecure accepts it
            pin_dev.update({"id": "53", "netTlsVerify": "verify"})
            hub.http_post("/api/device-save", pin_dev)
            st, r = hub.http_post("/api/device-net-test", {"id": "53", "command": "Ping"}, timeout=15)
            check(r.get("ok") is False and "TLS" in r.get("error", ""), "verify mode refuses a self-signed certificate", r)
            pin_dev.update({"id": "54", "netTlsVerify": "insecure"})
            hub.http_post("/api/device-save", pin_dev)
            st, r = hub.http_post("/api/device-net-test", {"id": "54", "command": "Ping"}, timeout=15)
            check(r.get("ok") is True and r.get("status") == 200, "insecure mode accepts it", r)

            # a stored token stays for the same address but never follows the device to another one
            moved = {k: v for k, v in pin_dev.items() if k not in ("httpToken", "commands")}
            hub.http_post("/api/device-save", moved)
            check(dev_by_id("54").get("hasHttpToken") is True, "token kept for the same host and port")
            moved["netPort"] = tls_port + 1
            hub.http_post("/api/device-save", moved)
            check(not dev_by_id("54").get("hasHttpToken"), "token dropped when the port changes", dev_by_id("54"))

            # a closed port: an error, answered within the timeout
            dead = {"id": "55", "name": "Dead", "type": "TCP", "netHost": "127.0.0.1", "netPort": 1,
                    "netTimeoutMs": 1000, "commands": [{"name": "X", "tcpPayload": "x"}]}
            hub.http_post("/api/device-save", dead)
            t0 = time.time()
            st, r = hub.http_post("/api/device-net-test", {"id": "55", "command": "X"}, timeout=15)
            check(r.get("ok") is False and time.time() - t0 < 5, "closed port reported", r)

            # the Hub itself: refused before anything is sent
            st, _ = hub.http_post("/api/device-save", {"id": "56", "name": "Self", "type": "HTTP", "netHost": "localhost",
                                                       "netPort": other.server_address[1]})
            check(st == 400, "localhost not saved", st)
            hub.http_post("/api/device-save", {"id": "56", "name": "Self", "type": "HTTP", "netHost": "2130706433",
                                               "netPort": other.server_address[1],
                                               "commands": [{"name": "Ping", "httpPath": "/ping"}]})
            n = len(REQUESTS)
            st, r = hub.http_post("/api/device-net-test", {"id": "56", "command": "Ping"}, timeout=15)
            check(r.get("ok") is False and "Hub itself" in r.get("error", ""), "2130706433 (127.0.0.1) refused", r)
            check(len(REQUESTS) == n, "nothing sent to it", REQUESTS[n:])
    finally:
        for s in (web, tls, other):
            try:
                s.shutdown()
                s.server_close()
            except Exception:
                pass
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("net device command: %d FAILED" % len(failures))
        return 1
    print("all net device command checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
