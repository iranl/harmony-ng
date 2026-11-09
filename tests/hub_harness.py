#!/usr/bin/env python3
"""
Host harness for hub_daemon tests: builds hub_daemon_test and the mock hardware library,
starts the daemon in dry-run mode on a temporary mock root (HARMONY_MOCK_DIR) with the sample
configuration from tests/fixtures/ha/sample_config, and gives the tests a plain-stdlib
WebSocket client and HTTP helpers.

Used by test_ws_events.py, test_ws_auth.py, test_ws_keepalive.py, test_ir_raw.py,
test_status.py and record_fixtures.py. Pure stdlib.
"""

import base64
import http.client
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(TESTS_DIR, ".."))
BUILD_DIR = os.path.join(REPO_ROOT, "build", "tmp", "test_host")
HUB_BIN = os.path.join(BUILD_DIR, "hub_daemon_test")
MOCK_LIB = os.path.join(BUILD_DIR, "libmock_hardware.so")
SAMPLE_CONFIG = os.path.join(TESTS_DIR, "fixtures", "ha", "sample_config")

# Sources of the host build of hub_daemon (keep in sync with build/compile_all.sh and
# tests/run_tests.sh)
HUB_DAEMON_SOURCES = [
    "hub_daemon.c", "http_server.c", "http_util.c", "http_auth.c", "http_api_auth.c",
    "http_api_activities.c", "http_api_devices.c", "http_api_commands.c",
    "http_api_custom_icons.c", "http_api_network.c", "hub_network_util.c",
    "http_api_remotes.c", "http_api_ota.c", "http_api_backup.c", "http_api_system.c",
    "config_store.c", "bt_migrate.c", "config_sanitize.c", "orchestrator.c",
    "orchestrator_queue.c", "hub_mqtt.c", "hub_mqtt_ha.c", "hub_ws.c", "hub_ntp.c",
    "resource_cache.c", "cJSON.c", "ir_encoder.c", "ir_i2s.c", "hw_action.c",
    "ws_server.c", "state_watch.c", "hub_events.c", "hub_info.c", "hub_mdns.c", "hub_ssdp.c", "hbus_config.c", "hbus_server.c", "hub_cmd.c", "mfg_id.c", "mqtt_client.c", "sha256.c",
    "timezone_db.c",
]


def _sources_newer_than(out):
    """True when any daemon source or header is newer than the binary (a stale test binary
    silently tests old code)."""
    try:
        out_mtime = os.path.getmtime(out)
    except OSError:
        return True
    src_dir = os.path.join(REPO_ROOT, "payload", "source")
    for name in os.listdir(src_dir):
        if name.endswith((".c", ".h")) and os.path.getmtime(os.path.join(src_dir, name)) > out_mtime:
            return True
    return False


def build_hub_daemon_test(force=False, extra_cflags=None, out=HUB_BIN):
    """Compile the host daemon with -DTEST_HARNESS=1 (when missing, when a source changed, or
    with force)."""
    if os.path.exists(out) and not force and not _sources_newer_than(out):
        return out
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = ["gcc", "-Wall", "-O2", "-g", "-DTEST_HARNESS=1", "-I" + os.path.join(REPO_ROOT, "payload", "source")]
    cmd += list(extra_cflags or [])
    cmd += [os.path.join(REPO_ROOT, "payload", "source", s) for s in HUB_DAEMON_SOURCES]
    cmd += ["-lpthread", "-lm", "-o", out]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        raise RuntimeError("hub_daemon_test compilation failed")
    return out


def build_mock_lib(force=False):
    src = os.path.join(TESTS_DIR, "mock_hardware", "mock_hardware.c")
    # rebuilt when the source is newer: a stale library tests old mock behaviour
    if os.path.exists(MOCK_LIB) and not force and os.path.getmtime(MOCK_LIB) >= os.path.getmtime(src):
        return MOCK_LIB
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = ["gcc", "-Wall", "-O2", "-fPIC", "-shared",
           os.path.join(TESTS_DIR, "mock_hardware", "mock_hardware.c"), "-ldl", "-o", MOCK_LIB]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        raise RuntimeError("libmock_hardware.so compilation failed")
    return MOCK_LIB


class HubHarness:
    """A hub_daemon_test on its own mock root. Use as a context manager."""

    def __init__(self, port=8997, hub_bin=None, env=None, args=None, config_dir=SAMPLE_CONFIG,
                 log_path=None, host="127.0.0.1", extra_config_files=None, extra_data_files=None,
                 extra_root_files=None):
        self.port = port
        # 127.0.0.2 is not trusted as loopback by http_auth, so auth tests use it
        self.host = host
        self.extra_config_files = dict(extra_config_files or {})
        self.extra_data_files = dict(extra_data_files or {})  # files under /data/harmony itself (mqtt.json)
        self.extra_root_files = dict(extra_root_files or {})  # files under /data (path relative to it)
        self.hub_bin = hub_bin or HUB_BIN
        self.extra_env = dict(env or {})
        self.extra_args = list(args or [])
        self.config_dir = config_dir
        self.mock_dir = None
        self.proc = None
        self.log_path = log_path
        self.log_file = None

    @property
    def base_url(self):
        return "http://%s:%d" % (self.host, self.port)

    @property
    def data_dir(self):
        """The daemon's /data/harmony as seen on the host"""
        return os.path.join(self.mock_dir, "data", "harmony")

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    def start(self):
        build_mock_lib()
        if self.hub_bin == HUB_BIN:
            build_hub_daemon_test()  # rebuilds when missing or older than a source
        elif not os.path.exists(self.hub_bin):
            raise RuntimeError("hub binary not found: %s" % self.hub_bin)
        self.mock_dir = tempfile.mkdtemp(prefix="hub_harness_")
        cfg = os.path.join(self.data_dir, "config")
        os.makedirs(cfg, exist_ok=True)
        for name in ("devices.json", "activities.json", "settings.json"):
            src = os.path.join(self.config_dir, name)
            if os.path.exists(src):
                shutil.copy(src, os.path.join(cfg, name))
        for name in ("remote_slot1_map.json", "remote_slot2_map.json"):
            src = os.path.join(self.config_dir, name)
            if os.path.exists(src):
                shutil.copy(src, os.path.join(self.data_dir, name))
        for name, content in self.extra_config_files.items():
            with open(os.path.join(cfg, name), "w") as f:
                f.write(content if isinstance(content, str) else json.dumps(content))
        for name, content in self.extra_data_files.items():
            with open(os.path.join(self.data_dir, name), "w") as f:
                f.write(content if isinstance(content, str) else json.dumps(content))
        for name, content in self.extra_root_files.items():
            path = os.path.join(self.mock_dir, "data", name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(content if isinstance(content, str) else json.dumps(content))
        env = os.environ.copy()
        env["HARMONY_MOCK_DIR"] = self.mock_dir
        env["HARMONY_MOCK_ROLE"] = "hub"
        env["LD_PRELOAD"] = MOCK_LIB
        env.update(self.extra_env)
        if self.log_path:
            self.log_file = open(self.log_path, "wb")
            out = self.log_file
        else:
            out = subprocess.DEVNULL
        self.proc = subprocess.Popen([self.hub_bin, "--dry-run", "--port", str(self.port)] + self.extra_args,
                                     env=env, stdout=out, stderr=subprocess.STDOUT)
        for _ in range(50):
            time.sleep(0.1)
            st, _ = self.http_get("/health")
            if st == 200:
                return
        self.stop()
        raise RuntimeError("hub_daemon_test did not come up on port %d" % self.port)

    def stop(self):
        if self.proc:
            try:
                self.proc.send_signal(signal.SIGKILL)
                self.proc.wait(timeout=5)
            except Exception:
                pass
            self.proc = None
        if self.log_file:
            self.log_file.close()
            self.log_file = None
        if self.mock_dir and os.path.isdir(self.mock_dir):
            shutil.rmtree(self.mock_dir, ignore_errors=True)
        self.mock_dir = None

    def http_get(self, path, headers=None, timeout=3.0):
        return _http(self.host, self.port, "GET", path, None, headers or {}, timeout)

    def http_post(self, path, payload, headers=None, timeout=3.0, raw=None):
        data = raw if raw is not None else json.dumps(payload).encode("utf-8")
        hdrs = {"Content-Type": "application/json"}
        hdrs.update(headers or {})
        return _http(self.host, self.port, "POST", path, data, hdrs, timeout)

    def ws(self, path="/ws", headers=None, timeout=5.0):
        return WsClient(self.host, self.port, path, headers=headers, timeout=timeout)


def source_address_for(host):
    """Connecting to 127.0.0.2 from the default source gives the peer 127.0.0.1, which the
    daemon trusts as loopback: bind the client to the same address to be seen as 127.0.0.2."""
    return (host, 0) if host != "127.0.0.1" else None


def _http(host, port, method, path, data, headers, timeout):
    """(status, parsed JSON or text); status 0 on a connection error"""
    try:
        conn = http.client.HTTPConnection(host, port, timeout=timeout, source_address=source_address_for(host))
        conn.request(method, path, body=data, headers=headers)
        r = conn.getresponse()
        body = r.read().decode("utf-8", "replace")
        conn.close()
        try:
            return r.status, json.loads(body)
        except ValueError:
            return r.status, body
    except Exception as e:
        return 0, str(e)


class WsHandshakeError(Exception):
    def __init__(self, status, headers):
        Exception.__init__(self, "WebSocket upgrade refused: %s" % status)
        self.status = status
        self.headers = headers


class WsClient:
    """Minimal RFC 6455 client: text frames, ping/pong, close. Client frames are masked."""

    def __init__(self, host, port, path="/ws", headers=None, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout, source_address=source_address_for(host))
        self.timeout = timeout
        self.buf = b""
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n" % (path, host, port, key))
        for k, v in (headers or {}).items():
            req += "%s: %s\r\n" % (k, v)
        req += "\r\n"
        self.sock.sendall(req.encode("utf-8"))
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            resp += chunk
        head, _, rest = resp.partition(b"\r\n\r\n")
        lines = head.decode("utf-8", "replace").split("\r\n")
        self.status = int(lines[0].split(" ")[1]) if len(lines[0].split(" ")) > 1 else 0
        self.headers = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, v = ln.split(":", 1)
                self.headers[k.strip().lower()] = v.strip()
        if self.status != 101:
            self.sock.close()
            raise WsHandshakeError(self.status, self.headers)
        self.buf = rest

    def _read_exact(self, n, deadline):
        while len(self.buf) < n:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise socket.timeout()
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("socket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_frame(self, timeout=None):
        """Returns (opcode, payload bytes) or None on timeout."""
        deadline = time.time() + (self.timeout if timeout is None else timeout)
        try:
            hdr = self._read_exact(2, deadline)
            opcode = hdr[0] & 0x0F
            masked = hdr[1] & 0x80
            ln = hdr[1] & 0x7F
            if ln == 126:
                ln = struct.unpack("!H", self._read_exact(2, deadline))[0]
            elif ln == 127:
                ln = struct.unpack("!Q", self._read_exact(8, deadline))[0]
            mask = self._read_exact(4, deadline) if masked else None
            data = self._read_exact(ln, deadline)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            return opcode, data
        except socket.timeout:
            return None

    def recv_json(self, timeout=None):
        """Next text frame parsed as JSON (pings are answered, pongs skipped); None on timeout."""
        deadline = time.time() + (self.timeout if timeout is None else timeout)
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            fr = self.recv_frame(remaining)
            if fr is None:
                return None
            op, data = fr
            if op == 0x1:
                try:
                    return json.loads(data.decode("utf-8", "replace"))
                except ValueError:
                    continue
            if op == 0x9:
                self.send_frame(0xA, data)
            if op == 0x8:
                raise ConnectionError("close frame")

    def wait_for(self, msg_type, timeout=5.0, where=None):
        """Collects frames until one with type == msg_type (and where(msg) true); returns
        (match, all frames seen including it), match None on timeout."""
        seen = []
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None, seen
            msg = self.recv_json(remaining)
            if msg is None:
                return None, seen
            seen.append(msg)
            if msg.get("type") == msg_type and (where is None or where(msg)):
                return msg, seen

    def drain(self, quiet=0.3):
        """Reads frames until none arrives for `quiet` seconds."""
        seen = []
        while True:
            msg = self.recv_json(quiet)
            if msg is None:
                return seen
            seen.append(msg)

    def send_frame(self, opcode, data=b""):
        mask = os.urandom(4)
        hdr = bytes([0x80 | opcode])
        ln = len(data)
        if ln < 126:
            hdr += bytes([0x80 | ln])
        elif ln < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack("!H", ln)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack("!Q", ln)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(hdr + mask + masked)

    def send_text(self, text):
        self.send_frame(0x1, text.encode("utf-8"))

    def send_json(self, obj):
        self.send_text(json.dumps(obj))

    def ping(self, data=b""):
        self.send_frame(0x9, data)

    def close(self):
        try:
            self.send_frame(0x8, b"")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


if __name__ == "__main__":
    build_mock_lib()
    build_hub_daemon_test(force="--force" in sys.argv)
    print(HUB_BIN)
