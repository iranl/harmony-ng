#!/usr/bin/env python3
"""
What hub_daemon hands to hub_btstack, checked on the way:
- Bluetooth addresses: POST /api/bt-connect, /api/bt-disconnect, /api/bt-link-device and
  /api/remote-pair answer 400 for a value that is not "XX:XX:XX:XX:XX:XX" (hub_btstack read an
  address it could not parse as "every host"; a stored one went in front of every key)
- WebSocket bt_key, bt_mouse and bt_text: a line break in key, button or target is refused (it
  started another hub_btstack command), target must be an address, text is at most 240 characters
- bt_key keyAction "release_all" releases (it tapped the key)
- the TARGET line and the command reach the key pipe in one write (two writes left a TARGET alone
  in a full pipe, and another writer's line could come between them)
- a button event without a slot (the Bluetooth remote, source "bt_remote") carries no slot (it was
  published as a key of RF remote slot 1)
- POST /api/bt-toggle keeps Bluetooth off with the supervisor's flag file
The daemon runs with --dry-run; the bt_key check reads /tmp/bthid_input, so run it under
flock /tmp/hng_suite.lock (it is skipped when another reader owns the pipe).
"""

import errno
import fcntl
import json
import os
import shutil
import stat
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, WsClient, SAMPLE_CONFIG  # noqa: E402

PORT = 9160
BOX = "00:11:22:33:44:01"  # test address, not a real device
PIPE = "/tmp/bthid_input"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def config_dir():
    d = tempfile.mkdtemp(prefix="bt_api_checks_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devices["devices"].append({
        "id": "3", "name": "Box", "type": "BT", "bdaddr": BOX,
        "commands": [{"name": "Home", "raw": "HID:0C:0223"}],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    for name in ("activities.json", "settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


def ws_ack(ws, req, seconds=3.0):
    ws.send_json(req)
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            m = ws.recv_json(timeout=max(0.1, deadline - time.time()))
        except Exception:
            continue
        if m and m.get("msgId") == req.get("msgId"):
            return m
    return None


def ws_event(ws, kind, seconds=3.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            m = ws.recv_json(timeout=max(0.1, deadline - time.time()))
        except Exception:
            continue
        if m and m.get("type") == kind:
            return m
    return None


def open_pipe_reader(path=PIPE):
    """A reader on the key pipe at path, or None when the path is taken by something else"""
    if os.path.exists(path):
        try:
            if not stat.S_ISFIFO(os.stat(path).st_mode):
                return None
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            return None
        # a FIFO with another reader (a running hub_btstack): do not take its lines
        os.close(fd)
        return None
    os.mkfifo(path, 0o600)
    return os.open(path, os.O_RDONLY | os.O_NONBLOCK)


def fill_pipe(path, room=30):
    """Fills the FIFO at path (its reader is open) up to room bytes in its last page; returns the
    writer to close afterwards. Linux keeps a pipe in pages: a small write goes into the room left
    in the last one, a write larger than that needs a new page and there is none."""
    try:
        fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return None
    size = fcntl.fcntl(fd, 1032)  # F_GETPIPE_SZ
    page = 4096
    for _ in range(size // page - 1):
        os.write(fd, b"x" * page)
    os.write(fd, b"x" * (page - room))
    return fd


def read_pipe(fd, seconds=1.0):
    out = b""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 4096)
            if chunk:
                out += chunk
                continue
        except OSError as e:
            if e.errno != errno.EAGAIN:
                break
        time.sleep(0.05)
    return out.decode("utf-8", "replace")


def main():
    cfg = config_dir()
    try:
        with HubHarness(port=PORT, config_dir=cfg) as hub:
            print("[-] addresses on the Bluetooth routes")
            for path, extra in (("/api/bt-connect", {}), ("/api/bt-disconnect", {}),
                                ("/api/bt-link-device", {"deviceId": "3"}), ("/api/remote-pair", {})):
                for bad in ("001122334401", "0:1:2:3:4:5", BOX + "\npair_host_on X"):
                    body = dict(extra, bdaddr=bad)
                    st, _ = hub.http_post(path, body)
                    check(st == 400, "%s %r: 400" % (path, bad), str(st))
            st, r = hub.http_post("/api/bt-disconnect", {"bdaddr": BOX})
            check(st == 200, "/api/bt-disconnect with an address: 200", str(st))
            st, r = hub.http_post("/api/bt-connect", {})
            check(st == 200, "/api/bt-connect without an address: 200", str(st))
            st, r = hub.http_post("/api/bt-link-device", {"deviceId": "3", "bdaddr": BOX})
            check(st == 200 and r.get("ok") is True, "/api/bt-link-device with an address: ok", str(r))

            print("[-] WebSocket bt_key, bt_mouse, bt_text")
            ws = WsClient("127.0.0.1", PORT)
            ws.recv_json(timeout=5)  # snapshot
            r = ws_ack(ws, {"action": "bt_key", "msgId": 1, "key": "up", "target": BOX + "\npair_host_on X"})
            check(r and r.get("ok") is False and r.get("code") == 400, "a line break in target: 400", str(r))
            r = ws_ack(ws, {"action": "bt_key", "msgId": 2, "key": "up\npair_host_on X"})
            check(r and r.get("ok") is False and r.get("code") == 400, "a line break in key: 400", str(r))
            r = ws_ack(ws, {"action": "bt_key", "msgId": 3, "key": "up", "target": "box"})
            check(r and r.get("ok") is False and r.get("code") == 400, "a target that is no address: 400", str(r))
            r = ws_ack(ws, {"action": "bt_mouse", "msgId": 4, "mouseAction": "down", "button": "left\nscan"})
            check(r and r.get("ok") is False and r.get("code") == 400, "a line break in the mouse button: 400", str(r))
            r = ws_ack(ws, {"action": "bt_text", "msgId": 5, "text": "a" * 241})
            check(r and r.get("ok") is False and r.get("code") == 400, "a text of 241 characters: 400", str(r))

            # the mock library may map /tmp/bthid_input into the mock root: listen on both
            mock_pipe = os.path.join(hub.mock_dir, "bthid_input")
            fd = open_pipe_reader()
            mock_fd = open_pipe_reader(mock_pipe)
            if fd is None:
                print("  SKIP the key pipe is in use: keyAction release_all not checked")
            else:
                def sent():
                    return read_pipe(fd) + (read_pipe(mock_fd, 0.05) if mock_fd is not None else "")
                try:
                    sent()  # the daemon's own release at start
                    r = ws_ack(ws, {"action": "bt_key", "msgId": 6, "key": "VolumeUp", "keyAction": "release_all", "target": BOX})
                    lines = sent()
                    check(r and r.get("ok") is True and ("TARGET %s\nRELEASE\n" % BOX) in lines and "KEY " not in lines,
                          "keyAction release_all sends RELEASE, not a tap", repr(lines))
                    r = ws_ack(ws, {"action": "bt_text", "msgId": 7, "text": "a" * 240, "target": BOX})
                    lines = sent()
                    check(r and r.get("ok") is True and ("TEXT " + "a" * 240 + "\n") in lines, "a text of 240 characters is sent", repr(lines[:60]))

                    # target and command in one write: with the pipe nearly full (room for the
                    # TARGET line, not for both) nothing is written; two writes left a TARGET
                    # behind, and the next writer's command went to that host
                    writers = [w for w in (fill_pipe(PIPE), fill_pipe(mock_pipe) if mock_fd is not None else None) if w is not None]
                    r = ws_ack(ws, {"action": "bt_key", "msgId": 8, "key": "Home", "target": BOX})
                    for w in writers:
                        os.close(w)
                    lines = sent().replace("x", "")
                    check(r and r.get("ok") is False and r.get("code") == 503 and "TARGET" not in lines,
                          "a full pipe takes the target and the key whole or not at all", "%s %r" % (r, lines))
                    r = ws_ack(ws, {"action": "bt_key", "msgId": 9, "key": "Home", "target": BOX})
                    lines = sent()
                    check(r and r.get("ok") is True and ("TARGET %s\nKEY Home\n" % BOX) in lines,
                          "with room again they arrive together", repr(lines))
                finally:
                    os.close(fd)
                    os.unlink(PIPE)
                    if mock_fd is not None:
                        os.close(mock_fd)
                        os.unlink(mock_pipe)

            print("[-] button events")
            st, _ = hub.http_post("/api/button-press", {"command": "OK", "action": "press", "source": "bt_remote"})
            ev = ws_event(ws, "button")
            check(st == 200 and ev is not None and "slot" not in ev and ev.get("source") == "bt_remote",
                  "a Bluetooth remote key has no slot", str(ev))
            st, _ = hub.http_post("/api/button-press", {"button": "OK", "action": "press", "source": "remote_rf", "slot": 1})
            ev = ws_event(ws, "button")
            check(ev is not None and ev.get("slot") == 1, "an RF remote key keeps its slot", str(ev))
            ws.close()

            print("[-] Bluetooth on and off")
            flag = os.path.join(hub.data_dir, "bt_disabled.conf")
            st, r = hub.http_post("/api/bt-toggle", {"enable": False})
            st2, sv = hub.http_get("/api/services")
            check(st == 200 and os.path.exists(flag) and sv.get("bt_enabled") is False,
                  "off writes the supervisor's flag file and reports disabled", "%s %s" % (r, sv))
            st, r = hub.http_post("/api/bt-toggle", {"enable": True})
            st2, sv = hub.http_get("/api/services")
            check(st == 200 and not os.path.exists(flag) and sv.get("bt_enabled") is True,
                  "on removes it (the supervisor starts hub_btstack)", "%s %s" % (r, sv))
            # no hub_btstack runs here: the answer says enabled, and not running yet (it said
            # running before the supervisor had started it)
            check(r.get("bt_enabled") is True and r.get("bt_running") is False,
                  "the answer to on reports the setting and the process as they are", str(r))
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
    if failed:
        print("[-] test_bt_api_checks: %d failed" % failed)
        sys.exit(1)
    print("[+] test_bt_api_checks: Bluetooth requests checked before they reach hub_btstack")


if __name__ == "__main__":
    main()
