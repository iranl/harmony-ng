#!/usr/bin/env python3
"""
Key events from hub_rf reach hub_daemon on one ordered channel (hub_key_channel.h): a UNIX
datagram per press, hold and release, read by one thread of hub_daemon. A press or hold older
than HUB_KEY_STALE_MS (300 ms) is skipped, a release always runs.

hub_rf used to send each key event as two HTTP requests (POST /api/ir-send, then
/api/button-press), give each up after 400 ms and go on. While hub_daemon's main loop was busy
the requests waited in the listen queue and ran seconds later, after hub_rf had given them up;
behind the first 8 the daemon answered 503 and the release of a held key could be lost.

Checks:
1. Datagrams sent straight to the channel: a fresh press runs (IR send + button event), a stale
   hold and a stale duplicate of the press are skipped, a stale release still runs; the button
   events come in order (press, release).
2. hub_rf's real key path (tests/rf_key_client.c: rf_keymap.c queue and dispatch thread) holds a
   key while the daemon's main loop is stalled for 7 s (a WebSocket frame that trickles in):
   the press and the holds run while they are fresh, nothing runs after the stall, and the
   button events are press then release. Before the change the IR sends all ran after the
   stall (late) and the release could be lost.
3. hub_rf against a hub_daemon without the key channel (no socket): it falls back to the HTTP
   routes and the key still arrives (press and release events).

The daemon is pinned to one CPU, as on the single-core Hub.
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, REPO_ROOT, BUILD_DIR, MOCK_LIB, SAMPLE_CONFIG  # noqa: E402

PORT = 8938
STALE_MS = 300
IR_LINE = b"[DRY-RUN] IR Keycode simulated"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def build_client():
    out = os.path.join(BUILD_DIR, "rf_key_client")
    src = os.path.join(REPO_ROOT, "payload", "source")
    os.makedirs(BUILD_DIR, exist_ok=True)
    subprocess.run(["gcc", "-Wall", "-O2", "-I" + src, os.path.join(REPO_ROOT, "tests", "rf_key_client.c"),
                    os.path.join(src, "rf_keymap.c"), os.path.join(src, "rf_state.c"), os.path.join(src, "cJSON.c"),
                    "-Wl,--wrap=http_post_local", "-lpthread", "-lm", "-o", out], check=True)
    return out


def devices_with_amp():
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        devices = json.load(f)
    devices["devices"].append({
        "id": "9", "name": "Amp", "type": "Amplifier", "manufacturer": "Example", "model": "A-1",
        "controlPort": 7,
        "commands": [{"name": "VolumeUp", "raw": "0000 006d 0022 0002 0157 00ac 0016 0016 ", "port": 7,
                      "repeats": 1}],
    })
    return devices


def ir_sends(log_path):
    with open(log_path, "rb") as f:
        return f.read().count(IR_LINE)


def log_text(log_path):
    with open(log_path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def button_actions(ws, quiet=1.0):
    return [m.get("action") for m in ws.drain(quiet) if m.get("type") == "button"]


def pin_one_cpu(pid):
    try:
        os.sched_setaffinity(pid, {sorted(os.sched_getaffinity(0))[0]})
    except (AttributeError, OSError):
        pass


def now_ms():
    return int(time.monotonic() * 1000)  # CLOCK_MONOTONIC, as hub_rf and hub_daemon


def send_dgram(path, obj):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(json.dumps(obj).encode(), path)
    finally:
        s.close()


def run_client(client, hub, sock_path, events, linger_ms):
    env = os.environ.copy()
    env["HARMONY_MOCK_DIR"] = hub.mock_dir
    env["HARMONY_MOCK_ROLE"] = "hub"
    env["LD_PRELOAD"] = MOCK_LIB
    lines = "".join("%d %s VolumeUp 9\n" % (wait, action) for wait, action in events)
    return subprocess.run([client, str(PORT), sock_path, str(linger_ms)], input=lines, text=True,
                          capture_output=True, env=env, timeout=30)


def stall_main_loop(staller, seconds):
    """Keeps the main loop of hub_daemon in a blocking read: a WebSocket frame whose payload
    trickles in, one byte every 0.5 s"""
    payload = (json.dumps({"action": "ping"}) + " " * 100)[:100].encode()
    staller.sock.sendall(bytes([0x81, 0x80 | len(payload)]) + b"\x00\x00\x00\x00")
    t0 = time.time()
    i = 0
    while time.time() - t0 < seconds and i < len(payload) - 1:
        staller.sock.sendall(payload[i:i + 1])
        i += 1
        time.sleep(0.5)
    staller.sock.sendall(payload[i:])


def test_channel_order_and_stale(tmp):
    print("[*] key channel: fresh press runs, stale press/hold skipped, stale release runs")
    sock_path = os.path.join(tmp, "keys1.sock")
    log_path = os.path.join(tmp, "daemon1.log")
    with HubHarness(port=PORT, log_path=log_path, args=["--key-socket", sock_path],
                    extra_config_files={"devices.json": devices_with_amp()}) as hub:
        pin_one_cpu(hub.proc.pid)
        check(os.path.exists(sock_path), "the daemon binds the key channel socket")
        if not os.path.exists(sock_path):
            return
        ws = hub.ws()
        ws.drain(0.3)
        base = {"slot": 1, "button": "VolumeUp", "deviceId": "9", "command": "VolumeUp"}
        old = now_ms() - 2000
        send_dgram(sock_path, dict(base, seq=1, ts=now_ms(), action="press"))
        send_dgram(sock_path, dict(base, seq=2, ts=old, action="hold"))
        send_dgram(sock_path, dict(base, seq=3, ts=old, action="press"))  # a late duplicate
        send_dgram(sock_path, dict(base, seq=4, ts=old, action="release"))
        actions = button_actions(ws)
        time.sleep(0.3)
        text = log_text(log_path)
        check(ir_sends(log_path) == 1, "only the fresh press sent the IR command", "sends=%d" % ir_sends(log_path))
        check(actions == ["press", "release"], "button events in order: press, release", str(actions))
        check("#2 VolumeUp hold skipped" in text and "#3 VolumeUp press skipped" in text,
              "the stale hold and the stale press are logged as skipped")
        check("#4 VolumeUp release skipped" not in text, "the stale release is not skipped")
        ws.close()


def test_hub_rf_hold_across_stall(tmp, client):
    print("[*] hub_rf key path: a held key while the main loop is stalled for 7 s")
    sock_path = os.path.join(tmp, "keys2.sock")
    log_path = os.path.join(tmp, "daemon2.log")
    with HubHarness(port=PORT, log_path=log_path, args=["--key-socket", sock_path],
                    extra_config_files={"devices.json": devices_with_amp()}) as hub:
        pin_one_cpu(hub.proc.pid)
        obs = hub.ws()
        obs.drain(0.3)
        staller = hub.ws()
        staller.drain(0.3)
        th = threading.Thread(target=stall_main_loop, args=(staller, 7.0))
        th.start()
        time.sleep(0.3)
        # press, three holds 150 ms apart, release: what the remote sends for a short hold
        res = run_client(client, hub, sock_path,
                         [(0, "press"), (150, "hold"), (150, "hold"), (150, "hold"), (150, "release")], 4000)
        during = ir_sends(log_path)
        stalled = th.is_alive()
        th.join()
        time.sleep(1.5)
        after = ir_sends(log_path) - during
        actions = button_actions(obs, 1.5)
        check(res.returncode == 0, "the hub_rf client ran", res.stderr[-300:])
        check(stalled, "the main loop was still stalled when the key was done")
        check(during == 4, "press and 3 holds sent while fresh, during the stall", "during=%d" % during)
        check(after == 0, "nothing sent late, after the stall", "after=%d" % after)
        check(actions == ["press", "release"], "button events: press, release", str(actions))
        obs.close()
        staller.close()


def test_http_fallback(tmp, client):
    print("[*] hub_rf against a daemon without the key channel: HTTP fallback")
    log_path = os.path.join(tmp, "daemon3.log")
    with HubHarness(port=PORT, log_path=log_path, extra_config_files={"devices.json": devices_with_amp()}) as hub:
        ws = hub.ws()
        ws.drain(0.3)
        res = run_client(client, hub, os.path.join(tmp, "missing.sock"), [(0, "press"), (100, "release")], 2500)
        actions = button_actions(ws)
        check(res.returncode == 0, "the hub_rf client ran", res.stderr[-300:])
        check(ir_sends(log_path) == 1, "the press sent the IR command over HTTP", "sends=%d" % ir_sends(log_path))
        check(actions == ["press", "release"], "button events: press, release", str(actions))
        ws.close()


def main():
    client = build_client()
    # a short directory: a UNIX socket path is limited to 107 bytes
    with tempfile.TemporaryDirectory(prefix="rfkey_", dir="/tmp" if os.path.isdir("/tmp") else None) as tmp:
        test_channel_order_and_stale(tmp)
        test_hub_rf_hold_across_stall(tmp, client)
        test_http_fallback(tmp, client)
    if failed:
        print("[-] test_rf_key_channel: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_rf_key_channel passed")


if __name__ == "__main__":
    main()
