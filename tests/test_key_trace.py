#!/usr/bin/env python3
"""
Key trace: with debug logging on, hub_daemon sends the times of a key from a remote to
hub_btstack in a "TS" line before the key's command, in the same write. hub_btstack turns it
into one line of /tmp/key_trace.log (tests/test_bt_key_trace.c). Without debug logging there
is no TS line and nothing else changes.

The daemon runs in dry-run on a mock root with a Bluetooth device; the test reads hub_btstack's
key pipe (tests/key_pipe_reader.py).

Checks:
1. Debug logging on (/data/harmony/debug_logging.conf): a press and its release through the key
   channel each carry "TS <rx> <daemon> <write>": rx is the time hub_rf gave the key, and
   rx <= daemon <= write. Target, TS and command arrive in one write.
2. Debug logging on: the same command through POST /api/ir-send carries no TS line (the trace
   is for keys from a remote).
3. Debug logging off: no TS line for a key from the key channel.
"""

import json
import os
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402
from key_pipe_reader import KeyPipeReader  # noqa: E402

PORT = 8948
BOX = "00:11:22:33:44:07"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def devices_with_bt():
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        devices = json.load(f)
    devices["devices"].append({
        "id": "7", "name": "Stream Box", "type": "BT", "manufacturer": "Example", "model": "S-1",
        "bdaddr": BOX, "btAutoReconnect": 0,
        "commands": [{"name": "DirectionUp", "keyCode": "HID:07:52"},
                     {"name": "Select", "keyCode": "HID:07:28"}],
    })
    return devices


def now_ms():
    return int(time.monotonic() * 1000)  # CLOCK_MONOTONIC, as hub_rf and hub_daemon


def send_key(path, seq, command, action, ts):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(json.dumps({"seq": seq, "ts": ts, "slot": 1, "button": command, "action": action,
                             "deviceId": "7", "command": command}).encode(), path)
    finally:
        s.close()


def ts_of(cmd):
    """The three times of a command's TS line, or None"""
    if len(cmd["extra"]) != 1:
        return None
    parts = cmd["extra"][0].split()
    if len(parts) != 4 or not all(p.isdigit() for p in parts[1:]):
        return None
    return [int(p) for p in parts[1:]]


def run(tmp, debug):
    sock_path = os.path.join(tmp, "keys%d.sock" % debug)
    extra = {"debug_logging.conf": ""} if debug else {}
    with HubHarness(port=PORT, args=["--key-socket", sock_path], extra_data_files=extra,
                    extra_config_files={"devices.json": devices_with_bt()}) as hub, \
            KeyPipeReader(hub.mock_dir) as pipe:
        if not os.path.exists(sock_path):
            check(False, "the daemon binds the key channel socket")
            return
        pipe.settle()
        t_press = now_ms() - 3
        send_key(sock_path, 1, "DirectionUp", "press", t_press)
        time.sleep(0.15)
        t_release = now_ms() - 3
        send_key(sock_path, 2, "DirectionUp", "release", t_release)
        cmds = pipe.wait(2)
        got = [c["command"] for c in cmds]
        if got != ["KBD_DOWN 52", "RELEASE"]:
            check(False, "press and release reach the pipe", str(got))
            return
        if not debug:
            check(all(not c["extra"] for c in cmds), "debug logging off: no TS line for a key from a remote",
                  str([c["extra"] for c in cmds]))
            return
        for c, rx, what in ((cmds[0], t_press, "press"), (cmds[1], t_release, "release")):
            ts = ts_of(c)
            check(ts is not None, "debug logging on: the %s carries a TS line with three times" % what, str(c["extra"]))
            if ts:
                check(ts[0] == rx and ts[0] <= ts[1] <= ts[2] and ts[2] - ts[0] < 1000,
                      "its times: rx as sent by hub_rf, then hub_daemon's two", "%s rx=%d" % (ts, rx))
        with pipe.lock:
            reads = list(pipe.reads)
        want = ("TARGET %s\nTS " % BOX).encode()
        check(any(r.startswith(want) and r.endswith(b"\nKBD_DOWN 52\n") and r.count(b"\n") == 3 for r in reads),
              "target, TS and command arrive in one write", str(reads))

        pipe.clear()
        time.sleep(0.3)
        st, body = hub.http_post("/api/ir-send", {"deviceId": "7", "command": "Select"})
        cmds = pipe.wait(1)
        check(st == 200 and [c["command"] for c in cmds] == ["KBD 28"] and not cmds[0]["extra"],
              "debug logging on: a command from the API carries no TS line",
              "status %s %s" % (st, [(c["command"], c["extra"]) for c in cmds]))


def main():
    # a short directory: a UNIX socket path is limited to 107 bytes
    with tempfile.TemporaryDirectory(prefix="keytrace_", dir="/tmp" if os.path.isdir("/tmp") else None) as tmp:
        print("[*] key trace: debug logging on")
        run(tmp, 1)
        print("[*] key trace: debug logging off")
        run(tmp, 0)
    if failed:
        print("[-] test_key_trace: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_key_trace passed")


if __name__ == "__main__":
    main()
