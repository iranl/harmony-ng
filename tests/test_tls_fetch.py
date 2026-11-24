#!/usr/bin/env python3
"""
hub_fetch (payload/source/hub_fetch.c, http_client.c, tls_client.c) on the host against local servers:

- HTTPS with a certificate from a test CA: accepted with that CA as bundle (CRLF line ends
  too, as a bundle copied from a Windows checkout has), refused with another bundle, refused
  when the certificate names another host; --insecure accepts any;
- Content-Length, chunked and read-until-close bodies; --max refuses a larger answer;
- redirects are followed; Authorization is dropped on a redirect to another host, and a
  redirect from https to http is refused;
- a header with CR or LF is refused; an HTTP error status exits 1 and removes the output.
"""

import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TESTS_DIR)
SRC = os.path.join(REPO, "payload", "source")
BUILD = os.path.join(REPO, "build", "tmp", "test_host")
sys.path.insert(0, TESTS_DIR)
from hub_harness import build_hub_fetch_test  # noqa: E402

SEEN = []


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        SEEN.append((self.headers.get("Host"), self.path, self.headers.get("Authorization")))
        p = self.path
        if p == "/plain":
            self._send(b"hello", length=True)
        elif p == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"abc", b"defgh", b"i"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        elif p == "/close":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"until-close")
            self.close_connection = True
        elif p == "/big":
            self._send(b"x" * 5000, length=True)
        elif p.startswith("/redir-other"):
            self.send_response(302)
            self.send_header("Location", "http://localhost:%d/plain" % self.server.server_address[1])
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif p == "/redir-long":
            self.send_response(302)
            self.send_header("Location", "/plain?" + "a" * 5000)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif p == "/redir-same":
            self.send_response(301)
            self.send_header("Location", "/plain")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif p == "/missing":
            self._send(b"no", length=True, code=404)
        else:
            self._send(b"?", length=True)

    def _send(self, body, length, code=200):
        self.send_response(code)
        if length:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def serve(cert=None):
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    if cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*cert)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def openssl(*args, cwd):
    subprocess.run(["openssl"] + list(args), cwd=cwd, check=True, capture_output=True)


def make_ca(d, name):
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", name + ".key", "-out", name + ".pem",
            "-days", "3", "-subj", "/CN=" + name, cwd=d)


def make_leaf(d, ca, name, dns):
    with open(os.path.join(d, name + ".ext"), "w") as f:
        f.write("subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth\n" % dns)
    openssl("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", name + ".key",
            "-out", name + ".csr", "-subj", "/CN=" + dns, cwd=d)
    openssl("x509", "-req", "-in", name + ".csr", "-CA", ca + ".pem", "-CAkey", ca + ".key", "-CAcreateserial",
            "-out", name + ".crt", "-days", "2", "-extfile", name + ".ext", cwd=d)
    return os.path.join(d, name + ".crt"), os.path.join(d, name + ".key")


class Fetch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hub_fetch = build_hub_fetch_test(force=True)
        cls.d = tempfile.mkdtemp(prefix="tls_fetch_")
        make_ca(cls.d, "ca")
        make_ca(cls.d, "other_ca")
        cls.ca = os.path.join(cls.d, "ca.pem")
        cls.other_ca = os.path.join(cls.d, "other_ca.pem")
        cls.ca_crlf = os.path.join(cls.d, "ca_crlf.pem")
        with open(cls.ca, "rb") as f, open(cls.ca_crlf, "wb") as g:
            g.write(f.read().replace(b"\n", b"\r\n"))
        cls.good = serve(make_leaf(cls.d, "ca", "good", "localhost"))
        cls.wrong = serve(make_leaf(cls.d, "ca", "wrong", "example.invalid"))
        cls.plain = serve()

    @classmethod
    def tearDownClass(cls):
        for s in (cls.good, cls.wrong, cls.plain):
            s.shutdown()
            s.server_close()
        shutil.rmtree(cls.d, ignore_errors=True)

    def fetch(self, url, *args):
        out = os.path.join(self.d, "out")
        if os.path.exists(out):
            os.unlink(out)
        r = subprocess.run([self.hub_fetch, "-T", "5", "-o", out] + list(args) + [url],
                           capture_output=True, text=True, timeout=30)
        body = None
        if os.path.exists(out):
            with open(out, "rb") as f:
                body = f.read()
        return r.returncode, body, r.stderr

    def test_ca_verify(self):
        url = "https://localhost:%d/plain" % self.good.server_address[1]
        self.assertEqual(self.fetch(url, "--ca", self.ca)[:2], (0, b"hello"))
        self.assertEqual(self.fetch(url, "--ca", self.ca_crlf)[:2], (0, b"hello"))
        rc, body, err = self.fetch(url, "--ca", self.other_ca)
        self.assertEqual((rc, body), (1, None))
        self.assertIn("not trusted", err)
        self.assertEqual(self.fetch(url, "--insecure")[:2], (0, b"hello"))

    def test_wrong_host_name(self):
        url = "https://localhost:%d/plain" % self.wrong.server_address[1]
        rc, _, err = self.fetch(url, "--ca", self.ca)
        self.assertEqual(rc, 1)
        self.assertIn("host name", err)

    def test_bodies(self):
        p = self.plain.server_address[1]
        self.assertEqual(self.fetch("http://127.0.0.1:%d/chunked" % p)[:2], (0, b"abcdefghi"))
        self.assertEqual(self.fetch("http://127.0.0.1:%d/close" % p)[:2], (0, b"until-close"))
        self.assertEqual(self.fetch("https://localhost:%d/chunked" % self.good.server_address[1], "--ca", self.ca)[:2],
                         (0, b"abcdefghi"))
        rc, body, err = self.fetch("http://127.0.0.1:%d/big" % p, "--max", "1000")
        self.assertEqual((rc, body), (1, None))
        self.assertIn("larger", err)
        rc, body, _ = self.fetch("http://127.0.0.1:%d/missing" % p)
        self.assertEqual((rc, body), (1, None))

    def test_redirects(self):
        p = self.plain.server_address[1]
        self.assertEqual(self.fetch("http://127.0.0.1:%d/redir-same" % p)[:2], (0, b"hello"))
        SEEN.clear()
        rc, body, _ = self.fetch("http://127.0.0.1:%d/redir-other" % p, "-H", "Authorization: token s3cret")
        self.assertEqual((rc, body), (0, b"hello"))
        self.assertEqual(SEEN[0][2], "token s3cret")
        self.assertIsNone(SEEN[-1][2], "no credentials for another host")
        self.assertTrue(SEEN[-1][0].startswith("localhost"))
        # a redirect target longer than the client takes is an error, never cut
        rc, body, err = self.fetch("http://127.0.0.1:%d/redir-long" % p)
        self.assertEqual((rc, body), (1, None))
        self.assertIn("redirect too long", err)
        # from https down to http: refused
        rc, body, err = self.fetch("https://127.0.0.1:%d/redir-other" % self.good.server_address[1], "--insecure")
        self.assertEqual((rc, body), (1, None))
        self.assertIn("https to http", err)

    def test_connect_to(self):
        """--connect-to: the caller's checked address, no lookup of the name; the name still
        goes in Host and is checked against the certificate"""
        p = self.plain.server_address[1]
        rc, _, err = self.fetch("http://name.invalid:%d/plain" % p)
        self.assertEqual(rc, 1)
        SEEN.clear()
        self.assertEqual(self.fetch("http://name.invalid:%d/plain" % p, "--connect-to", "192.0.2.1,127.0.0.1",
                                    "-T", "1")[:2], (0, b"hello"))
        self.assertEqual(SEEN[-1][0], "name.invalid:%d" % p)
        url = "https://example.invalid:%d/plain" % self.wrong.server_address[1]
        self.assertEqual(self.fetch(url, "--ca", self.ca, "--connect-to", "127.0.0.1")[:2], (0, b"hello"))
        rc, _, err = self.fetch("https://localhost:%d/plain" % self.wrong.server_address[1], "--ca", self.ca,
                                "--connect-to", "127.0.0.1")
        self.assertEqual(rc, 1)
        self.assertIn("host name", err)
        rc, _, err = self.fetch("http://127.0.0.1:%d/plain" % p, "--connect-to", "localhost")
        self.assertEqual(rc, 1)
        self.assertIn("bad address", err)

    def test_bad_header(self):
        rc, _, err = self.fetch("http://127.0.0.1:%d/plain" % self.plain.server_address[1], "-H", "X-A: a\r\nB: c")
        self.assertEqual(rc, 1)
        self.assertIn("bad header", err)

    def test_usage(self):
        self.assertEqual(subprocess.run([self.hub_fetch], capture_output=True).returncode, 2)
        self.assertEqual(subprocess.run([self.hub_fetch, "--probe"], capture_output=True).returncode, 0)

    def test_tcp_tls_and_status_file(self):
        """--tcp --tls with --pin-any reports the key; --pin with another key is refused"""
        status = os.path.join(self.d, "status")
        port = str(self.good.server_address[1])
        r = subprocess.run([self.hub_fetch, "--tcp", "--tls", "--pin-any", "--status-file", status,
                            "-o", os.path.join(self.d, "tcpout"), "127.0.0.1", port], capture_output=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(status) as f:
            fields = dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
        self.assertEqual(len(fields["fingerprint"]), 64)
        r = subprocess.run([self.hub_fetch, "--tcp", "--tls", "--pin", "00" * 32, "--status-file", status,
                            "-o", os.path.join(self.d, "tcpout"), "127.0.0.1", port], capture_output=True, timeout=30)
        self.assertEqual(r.returncode, 1)
        with open(status) as f:
            self.assertIn("pinned", f.read())


if __name__ == "__main__":
    unittest.main()
