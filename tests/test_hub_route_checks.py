#!/usr/bin/env python3
"""
Route checks of hub_daemon (port 9101, own mock root, clients bound to 127.0.0.2, auth off):

 1  the key map and layout saves refuse a body their readers would not load (not JSON, not an
    object, empty, larger than the readers take) and keep the stored file
 2  custom icons: an image over 16 KB is refused (was saved cut), the number of icons is
    limited, and delete removes only icons the upload route made (not a stock icon) and, with
    them, only their own older name
 3  query parameters are matched by their whole name and end at '&' (logs source, rf-mapping
    slot, device delete id)
 4  GET /api/logs reads the files the Hub writes (harmony-init.log, harmony-network.log)
 5  factory reset does not answer "initiated" when its script is not installed
 6  POST /api/settings refuses a timezone name the Hub does not know (it ran as CET)
 7  mqtt.json, which holds the broker password, is made readable by its owner only
"""

import base64
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 9101
HOST = "127.0.0.2"
failed = 0
PNG_SIG = b"\x89PNG\r\n\x1a\n"


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def raw(method, path, body=b""):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST, 0))
    s.settimeout(8)
    s.connect((HOST, PORT))
    s.sendall(("%s %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n"
               % (method, path, HOST, PORT, len(body))).encode() + body)
    out = b""
    try:
        while True:
            c = s.recv(65536)
            if not c:
                break
            out += c
    except (socket.timeout, ConnectionResetError):
        pass
    s.close()
    try:
        st = int(out.split(b" ", 2)[1])
    except (IndexError, ValueError):
        st = 0
    b = out.partition(b"\r\n\r\n")[2]
    try:
        return st, json.loads(b)
    except ValueError:
        return st, b.decode("utf-8", "replace")


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def test_map_saves():
    print("[-] 1  key map and layout saves")
    with HubHarness(port=PORT, host=HOST) as hub:
        layouts = os.path.join(hub.data_dir, "web_remote_layouts.json")
        slot1 = os.path.join(hub.data_dir, "remote_slot1_map.json")
        btmap = os.path.join(hub.data_dir, "bt_remote_map.json")
        st, _ = raw("POST", "/api/ui-remote-layout", json.dumps({"layouts": {"a": {"rows": 1}}}).encode())
        check(st == 200, "valid layout saved", str(st))
        good_layout = read(layouts)
        st, _ = raw("POST", "/api/ui-remote-layout", b"this is not JSON <<<")
        check(st == 400, "non-JSON layout refused", str(st))
        check(read(layouts) == good_layout, "stored layout kept")
        st, _ = raw("POST", "/api/ui-remote-layout", json.dumps({"layouts": {"big": "x" * 270000}}).encode())
        check(st == 413, "layout larger than the reader takes refused", str(st))
        check(read(layouts) == good_layout, "stored layout kept after the large one")
        before = read(slot1)
        st, _ = raw("POST", "/api/rf-mapping-save", b'{"activities": {"1": {"Vol')
        check(st == 400, "cut key map refused", str(st))
        check(read(slot1) == before, "stored slot 1 map kept")
        st, _ = raw("POST", "/api/rf-mapping-save", b'[1, 2, 3]')
        check(st == 400, "key map that is not an object refused", str(st))
        st, _ = raw("POST", "/api/remote-mapping-save", b"garbage")
        check(st == 400, "garbage BLE map refused", str(st))
        check(read(btmap) is None or read(btmap) != b"garbage", "garbage not stored")
        st, _ = raw("POST", "/api/remote-mapping-save", b"")
        check(st == 400, "empty body refused", str(st))
        st, _ = raw("POST", "/api/rf-mapping-save?slot=2", json.dumps({"activities": {}}).encode())
        check(st == 200 and read(os.path.join(hub.data_dir, "remote_slot2_map.json")) is not None,
              "valid slot 2 map saved", str(st))


def upload(name, data):
    return raw("POST", "/api/custom-icons", json.dumps(
        {"name": name, "image_base64": "data:image/png;base64," + base64.b64encode(data).decode()}).encode())


def test_icons():
    print("[-] 2  custom icons")
    tag = "p%d" % os.getpid()
    tmp_icons = "/tmp/www/assets/icons"
    made = []
    with HubHarness(port=PORT, host=HOST, extra_root_files={"harmony/custom_icons/.keep": ""}) as hub:
        cache_icons = os.path.join(hub.mock_dir, "cache", "www", "assets", "icons")
        os.makedirs(cache_icons, exist_ok=True)
        custom = os.path.join(hub.data_dir, "custom_icons")
        stock = os.path.join(cache_icons, "stock%s.png" % tag)
        with open(stock, "wb") as f:
            f.write(PNG_SIG + b"stock")

        st, r = upload("big" + tag, PNG_SIG + b"\0" * 20000)
        check(st == 413, "20 KB image refused", "%s %s" % (st, r))
        check(not os.path.exists(os.path.join(custom, "custom_big%s.png" % tag)), "nothing saved for it")

        st, r = upload("ok" + tag, PNG_SIG + b"\0" * 500)
        made.append("custom_ok%s.png" % tag)
        saved = read(os.path.join(custom, "custom_ok%s.png" % tag))
        check(st == 200 and saved is not None and len(saved) == 508, "small image saved whole", "%s %s" % (st, r))

        st, _ = raw("DELETE", "/api/custom-icons?filename=stock%s.png" % tag)
        check(os.path.exists(stock), "stock icon not deleted", str(st))
        st, _ = raw("DELETE", "/api/custom-icons?filename=..%2Fx.png")
        check(st == 400, "name outside the pattern refused", str(st))
        st, _ = raw("DELETE", "/api/custom-icons?filename=custom_ok%s.png&_=1" % tag)
        check(st == 200 and not os.path.exists(os.path.join(custom, "custom_ok%s.png" % tag)),
              "uploaded icon deleted (extra query parameter)", str(st))

        # The older name of the same icon goes with it; another icon that shares the part after
        # the first '_' stays (deleting "activity_watch_tv.png" removed "watch_tv.png")
        for n in ("activity_watch_tv%s.png" % tag, "watch_tv%s.png" % tag,
                  "activity_77%s.png" % tag, "77%s.png" % tag):
            with open(os.path.join(custom, n), "wb") as f:
                f.write(PNG_SIG + b"\0" * 10)
        st, _ = raw("DELETE", "/api/custom-icons?filename=activity_watch_tv%s.png" % tag)
        check(st == 200 and not os.path.exists(os.path.join(custom, "activity_watch_tv%s.png" % tag)),
              "icon deleted by file name", str(st))
        check(os.path.exists(os.path.join(custom, "watch_tv%s.png" % tag)),
              "other icon sharing the name after '_' kept")
        st, _ = raw("DELETE", "/api/custom-icons?id=77%s&type=activity" % tag)
        check(st == 200 and not os.path.exists(os.path.join(custom, "activity_77%s.png" % tag))
              and not os.path.exists(os.path.join(custom, "77%s.png" % tag)),
              "icon deleted by id, its older name too", str(st))
        os.unlink(os.path.join(custom, "watch_tv%s.png" % tag))

        for i in range(128):
            with open(os.path.join(custom, "custom_fill_%03d.png" % i), "wb") as f:
                f.write(PNG_SIG + b"\0" * 100)
        st, r = upload("more" + tag, PNG_SIG + b"\0" * 100)
        made.append("custom_more%s.png" % tag)
        check(st == 507, "no new icon past the limit", "%s %s" % (st, r))
        st, r = upload("fill_000", PNG_SIG + b"\0" * 100)
        check(st == 200, "replacing an existing icon is not a new one", str(st))
    for n in made + ["custom_fill_000.png"]:
        try:
            os.unlink(os.path.join(tmp_icons, n))
        except OSError:
            pass


def test_query_params():
    print("[-] 3  query parameters, 4  log files")
    with HubHarness(port=PORT, host=HOST, extra_data_files={"mqtt.json": {"enabled": False, "password": "x"}}) as hub:
        mode = os.stat(os.path.join(hub.data_dir, "mqtt.json")).st_mode & 0o777
        check(mode == 0o600, "mqtt.json (broker password) readable by its owner only", oct(mode))
        st, r = raw("GET", "/api/logs?source=rf&_=123")
        check(st == 200 and isinstance(r, dict) and r.get("source") == "rf", "source=rf with an extra parameter",
              repr(r)[:80])
        st, r = raw("GET", "/api/logs?source=rfoo")
        check(isinstance(r, dict) and r.get("source") == "daemon", "source=rfoo is not rf", repr(r)[:80])
        with open(os.path.join(hub.data_dir, "remote_slot2_map.json"), "w") as f:
            json.dump({"activities": {"77": {"VolumeUp": {"deviceId": "1", "command": "VolumeUp"}}}}, f)
        st, r = raw("GET", "/api/rf-mapping?slot=25")
        txt = json.dumps(r)
        check('"77"' not in txt, "slot=25 is not slot 2", txt[:80])
        st, r = raw("GET", "/api/rf-mapping?x=1&slot=2")
        check('"77"' in json.dumps(r), "slot=2 after another parameter", json.dumps(r)[:80])

        marks = {}
        for path in ("/tmp/harmony-init.log", "/tmp/harmony-network.log"):
            if os.path.exists(path):
                continue
            marks[path] = "MARK-%s-%d" % (os.path.basename(path), os.getpid())
            with open(path, "w") as f:
                f.write(marks[path] + "\n")
        try:
            if "/tmp/harmony-init.log" in marks:
                st, r = raw("GET", "/api/logs")
                check(marks["/tmp/harmony-init.log"] in (r.get("logs", "") if isinstance(r, dict) else ""),
                      "daemon log is harmony-init.log")
            else:
                print("  SKIP daemon log check (/tmp/harmony-init.log exists on this host)")
            if "/tmp/harmony-network.log" in marks:
                st, r = raw("GET", "/api/logs?source=network")
                check(marks["/tmp/harmony-network.log"] in (r.get("logs", "") if isinstance(r, dict) else ""),
                      "network log is harmony-network.log")
            else:
                print("  SKIP network log check (/tmp/harmony-network.log exists on this host)")
        finally:
            for path in marks:
                os.unlink(path)

        print("[-] 6  timezones")
        st, r = raw("POST", "/api/settings", json.dumps({"timezone": "America/Argentina/Buenos_Aires"}).encode())
        check(st == 400, "unlisted Area/City timezone refused", "%s %s" % (st, r))
        st, r = raw("POST", "/api/settings", json.dumps({"timezone": "Europe/Rome"}).encode())
        check(st == 200 and isinstance(r, dict) and r.get("timezone") == "Europe/Rome", "listed timezone saved", str(st))

        print("[-] 5  factory reset")
        st, r = raw("POST", "/system", json.dumps({"action": "factory_reset", "confirm": "RESET"}).encode())
        check(st == 500, "missing script reported, not 'initiated'", "%s %s" % (st, r))


def main():
    test_map_saves()
    test_icons()
    test_query_params()
    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] Route checks: all checks passed")


if __name__ == "__main__":
    main()
