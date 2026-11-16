#!/usr/bin/env python3
"""
Guards of tests/hub_harness.py.

 1  HubHarness.start() refuses a port another process already answers on. Before, the new
    daemon failed to bind and exited, the other process answered GET /health, and the test ran
    against the wrong daemon (wrong configuration, wrong mock root) without noticing.
 2  build_hub_daemon_test() rebuilds when the compile command changed (other flags), not only
    when a source is newer: a binary left by a coverage or sanitizer build must not stand in for
    the plain one. The compiler is replaced by a stub here, so nothing is built.
"""

import http.server
import os
import socketserver
import sys
import tempfile
import threading
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hub_harness  # noqa: E402

PORT = 9265
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


class Healthy(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def test_port_in_use():
    print("[-] 1  a port answered by another process is refused")
    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.TCPServer(("127.0.0.1", PORT), Healthy)
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    hub = hub_harness.HubHarness(port=PORT)
    try:
        try:
            hub.start()
            started = True
        except RuntimeError as e:
            started = False
            print("    start() refused: %s" % e)
        check(not started, "start() does not run the test against another process")
    finally:
        hub.stop()
        srv.shutdown()
        srv.server_close()


def test_rebuild_on_flags():
    print("[-] 2  a binary built with other flags is rebuilt")
    calls = []

    def fake_run(cmd, capture_output=True, text=True):
        calls.append(list(cmd))
        out = cmd[cmd.index("-o") + 1]
        with open(out, "w") as f:
            f.write("stub")
        return types.SimpleNamespace(returncode=0, stderr="", stdout="")

    real_run = hub_harness.subprocess.run
    hub_harness.subprocess.run = fake_run
    tmp = tempfile.mkdtemp(prefix="harness_guard_")
    out = os.path.join(tmp, "hub_daemon_test")
    try:
        hub_harness.build_hub_daemon_test(out=out, extra_cflags=["--coverage"])
        check(len(calls) == 1, "first build compiles", str(len(calls)))
        hub_harness.build_hub_daemon_test(out=out, extra_cflags=["--coverage"])
        check(len(calls) == 1, "same command, nothing newer: binary reused", str(len(calls)))
        hub_harness.build_hub_daemon_test(out=out)
        check(len(calls) == 2, "other flags: rebuilt", str(len(calls)))
    finally:
        hub_harness.subprocess.run = real_run
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)


def main():
    test_port_in_use()
    test_rebuild_on_flags()
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] harness guards: all checks passed")


if __name__ == "__main__":
    main()
