#!/usr/bin/env python3
"""
A key that just came from a remote is sent at once; the gaps between commands stay for
everything else.

hw_action.c waits before a press or tap when the command before it ended less than
interKeyDelay (100 ms, same device; 200 ms for the same command, holdInterKeyDelay) or
interDeviceDelay (500 ms, another device) ago. That is
right for a sequence a program sends back to back. For a key the user presses on a remote it
only added delay: two quick keys on one device came out 100 ms apart, and a key for another
device (volume on the amplifier, then a direction on the player) waited half a second.

The daemon runs in dry-run on a mock root with two Bluetooth devices; the test reads
hub_btstack's key pipe (tests/key_pipe_reader.py) and notes when each command arrives.

Checks:
1. Key channel (hub_key_channel.h), as hub_rf sends keys: two presses for one device 20 ms
   apart and a press for another device right after arrive without the 100 / 500 ms gaps; the
   same key pressed again after 120 ms is not held back to 200 ms (holdInterKeyDelay), and a
   duplicate 30 ms after a press is still dropped (the 90 ms filter is not changed).
2. The same commands through POST /api/ir-send keep the gaps.
3. A press that is 500 ms old is sent, one that is 1500 ms old is skipped (HUB_KEY_STALE_MS is
   1000; it was 300), and an old release always runs.
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

PORT = 8947
BOX = "00:11:22:33:44:07"
CONSOLE = "00:11:22:33:44:08"
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
                     {"name": "DirectionDown", "keyCode": "HID:07:51"},
                     {"name": "Select", "keyCode": "HID:07:28"}],
    })
    devices["devices"].append({
        "id": "8", "name": "Console", "type": "BT", "manufacturer": "Example", "model": "C-1",
        "bdaddr": CONSOLE, "btAutoReconnect": 0,
        "commands": [{"name": "Home", "keyCode": "HID:0C:223"}],
    })
    return devices


def now_ms():
    return int(time.monotonic() * 1000)  # CLOCK_MONOTONIC, as hub_rf and hub_daemon


def send_key(path, seq, device, command, action, age_ms=0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(json.dumps({"seq": seq, "ts": now_ms() - age_ms, "slot": 1, "button": command,
                             "action": action, "deviceId": device, "command": command}).encode(), path)
    finally:
        s.close()


def gaps(cmds):
    return [round(cmds[i + 1]["ms"] - cmds[i]["ms"], 1) for i in range(len(cmds) - 1)]


def main():
    # a short directory: a UNIX socket path is limited to 107 bytes
    with tempfile.TemporaryDirectory(prefix="livekey_", dir="/tmp" if os.path.isdir("/tmp") else None) as tmp:
        sock_path = os.path.join(tmp, "keys.sock")
        log_path = os.path.join(tmp, "daemon.log")
        with HubHarness(port=PORT, log_path=log_path, args=["--key-socket", sock_path],
                        extra_config_files={"devices.json": devices_with_bt()}) as hub, \
                KeyPipeReader(hub.mock_dir) as pipe:
            if not os.path.exists(sock_path):
                check(False, "the daemon binds the key channel socket")
                return finish()

            check(pipe.settle(), "the daemon's release-all after start reaches the pipe")

            print("[*] key channel: three live keys, two devices")
            send_key(sock_path, 1, "7", "DirectionUp", "press")
            time.sleep(0.020)
            send_key(sock_path, 2, "7", "DirectionDown", "press")
            send_key(sock_path, 3, "8", "Home", "press")
            cmds = pipe.wait(3)
            got = [(c["target"], c["command"]) for c in cmds]
            check(got == [(BOX, "KBD_DOWN 52"), (BOX, "KBD_DOWN 51"), (CONSOLE, "CONSUMER_DOWN 223")],
                  "the three presses reach the pipe in order, each with its host", str(got))
            if len(cmds) == 3:
                g = gaps(cmds)
                check(g[0] < 60, "second key of the same device: no 100 ms interKeyDelay", "gap %.1f ms" % g[0])
                check(g[1] < 60, "key for another device: no 500 ms interDeviceDelay", "gap %.1f ms" % g[1])
                print("       gaps between the keys in the pipe: %.1f ms, %.1f ms" % (g[0], g[1]))
            send_key(sock_path, 4, "7", "DirectionDown", "release")
            send_key(sock_path, 5, "8", "Home", "release")
            pipe.wait(5)

            print("[*] key channel: the same key twice")
            time.sleep(1.0)
            pipe.clear()
            send_key(sock_path, 10, "7", "Select", "press")
            time.sleep(0.030)
            send_key(sock_path, 11, "7", "Select", "press")   # 30 ms after: a duplicate
            time.sleep(0.090)
            send_key(sock_path, 12, "7", "Select", "press")   # 120 ms after: a second press
            cmds = pipe.wait(2)
            time.sleep(0.3)
            cmds = pipe.wait(0)
            check([c["command"] for c in cmds] == ["KBD_DOWN 28", "KBD_DOWN 28"],
                  "a press 30 ms after the same press is still dropped (90 ms duplicate filter)",
                  str([c["command"] for c in cmds]))
            if len(cmds) == 2:
                g = gaps(cmds)
                check(g[0] < 160, "the same key again after 120 ms: no 200 ms holdInterKeyDelay", "gap %.1f ms" % g[0])
                print("       gap between the two presses in the pipe: %.1f ms" % g[0])
            send_key(sock_path, 13, "7", "Select", "release")
            pipe.wait(3)

            print("[*] POST /api/ir-send: the same commands keep the gaps")
            time.sleep(1.0)
            pipe.clear()
            for dev, cmd in (("7", "DirectionUp"), ("7", "DirectionDown"), ("8", "Home")):
                st, body = hub.http_post("/api/ir-send", {"deviceId": dev, "command": cmd})
                if st != 200:
                    check(False, "POST /api/ir-send %s %s" % (dev, cmd), "status %s %s" % (st, body))
            cmds = pipe.wait(3)
            got = [(c["target"], c["command"]) for c in cmds]
            check(got == [(BOX, "KBD 52"), (BOX, "KBD 51"), (CONSOLE, "CONSUMER 223")],
                  "the three commands reach the pipe in order", str(got))
            if len(cmds) == 3:
                g = gaps(cmds)
                check(g[0] >= 95, "same device: interKeyDelay (100 ms) kept", "gap %.1f ms" % g[0])
                check(g[1] >= 490, "another device: interDeviceDelay (500 ms) kept", "gap %.1f ms" % g[1])
                print("       gaps between the commands in the pipe: %.1f ms, %.1f ms" % (g[0], g[1]))

            print("[*] key channel: a late press")
            time.sleep(1.0)
            pipe.clear()
            send_key(sock_path, 6, "7", "Select", "press", age_ms=1500)
            send_key(sock_path, 7, "7", "DirectionUp", "press", age_ms=500)
            send_key(sock_path, 8, "7", "DirectionUp", "release", age_ms=5000)
            cmds = pipe.wait(2, timeout=1.5)
            time.sleep(0.2)
            got = [c["command"] for c in pipe.wait(0)]
            with open(log_path, "rb") as f:
                text = f.read().decode("utf-8", "replace")
            check("KBD_DOWN 52" in got, "a press 500 ms old is sent", str(got))
            check("KBD_DOWN 28" not in got and "#6 Select press skipped" in text,
                  "a press 1500 ms old is skipped and logged", str(got))
            check(got[-1:] == ["RELEASE"] and "#8 DirectionUp release skipped" not in text,
                  "a release 5000 ms old still runs", str(got))
    return finish()


def finish():
    if failed:
        print("[-] test_live_key_gaps: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_live_key_gaps passed")


if __name__ == "__main__":
    main()
