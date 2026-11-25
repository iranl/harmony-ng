#!/usr/bin/env python3
"""
Issue 294, hub_daemon's side of PIN pairing and the PS3 profile:
- POST /api/bt-pairing {"profile": "ps3"} sends "pair_host_on profile=ps3 <name>"; the keyboard
  (no profile, or "keyboard") keeps the old "pair_host_on <name>" line; another profile is 400
- a line break in the pairing name does not start a second hub_btstack command
- POST /api/bt-pairing-pin: a PIN is a string of 1 to 16 digits (a number, letters, 17 digits or
  nothing are 400); 503 while hub_btstack is not running; with it running "pin_reply <digits>"
  and {"cancel": true} "pin_cancel" are sent
- a device command PS3:<hex> becomes PS3_DOWN / RELEASE / PS3 lines on the key pipe
The daemon runs with --dry-run (its hub_btstack commands are printed). A short lived process
named hub_btstack stands in for a running one (hub_daemon finds it in /proc).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402
from test_bt_api_checks import PIPE, open_pipe_reader, read_pipe  # noqa: E402

PORT = 9163
PS3 = "00:11:22:33:44:03"  # test address, not a real device
failed = 0


def check(cond, name, detail=""):
    global failed
    print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
    if not cond:
        failed += 1


def config_dir():
    d = tempfile.mkdtemp(prefix="bt_pin_api_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devices["devices"].append({
        "id": "3", "name": "PlayStation", "type": "BT", "bdaddr": PS3,
        "commands": [{"name": "PS", "raw": "PS3:43"}, {"name": "Triangle", "raw": "ps3:5c"},
                     {"name": "Bad", "raw": "PS3:5cx"}],
    })
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    for name in ("activities.json", "settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


class FakeBtstack:
    """A process whose name is hub_btstack, as hub_daemon looks for it in /proc"""

    def __enter__(self):
        self.dir = tempfile.mkdtemp(prefix="bt_pin_proc_")
        exe = os.path.join(self.dir, "hub_btstack")
        shutil.copy(shutil.which("sleep"), exe)
        self.proc = subprocess.Popen([exe, "60"])
        time.sleep(2.2)  # hub_daemon keeps its answer for 2 s
        return self

    def __exit__(self, *exc):
        self.proc.kill()
        self.proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)
        return False


def sent_since(log_path, mark):
    lines = open(log_path, errors="replace").read().splitlines()[mark:]
    return [l.split("BTstack command simulated: ", 1)[1].strip() for l in lines if "BTstack command simulated: " in l]


def log_len(log_path):
    return len(open(log_path, errors="replace").read().splitlines())


def main():
    cfg = config_dir()
    log_path = os.path.join(cfg, "hub.log")
    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log_path) as hub:
            print("[-] POST /api/bt-pairing profile")
            mark = log_len(log_path)
            st, r = hub.http_post("/api/bt-pairing", {"enable": True, "deviceId": "3", "profile": "ps3"})
            check(st == 200 and r.get("profile") == "ps3", "profile ps3: 200", "%s %s" % (st, r))
            st, r = hub.http_post("/api/bt-pairing", {"enable": True, "deviceId": "3", "name": "Living Room"})
            check(st == 200, "no profile: 200", str(st))
            st, r = hub.http_post("/api/bt-pairing", {"enable": True, "deviceId": "3", "profile": "keyboard",
                                                      "name": "Box\npair_host_off"})
            check(st == 200, "profile keyboard: 200", str(st))
            st, _ = hub.http_post("/api/bt-pairing", {"enable": True, "deviceId": "3", "profile": "wii"})
            check(st == 400, "an unknown profile: 400", str(st))
            time.sleep(0.3)
            sent = sent_since(log_path, mark)
            check("pair_host_on profile=ps3 Harmony Keyboard" in sent, "ps3: pair_host_on profile=ps3 <name>", str(sent))
            check("pair_host_on Living Room" in sent, "the keyboard keeps the old line", str(sent))
            check("pair_host_on Box pair_host_off" in sent and "pair_host_off" not in sent,
                  "a line break in the name is a blank, not a second command", str(sent))
            check(not any("wii" in s for s in sent), "nothing is sent for an unknown profile", str(sent))

            print("[-] POST /api/bt-pairing-pin")
            for bad in ({"pin": 842631}, {"pin": "84a631"}, {"pin": ""}, {"pin": "1" * 17}, {}, {"pin": "12\npair_host_off"}):
                st, _ = hub.http_post("/api/bt-pairing-pin", bad)
                check(st == 400, "%r: 400" % (bad,), str(st))
            st, _ = hub.http_post("/api/bt-pairing-pin", {"pin": "842631"})
            check(st == 503, "hub_btstack not running: 503", str(st))
            with FakeBtstack():
                mark = log_len(log_path)
                st, r = hub.http_post("/api/bt-pairing-pin", {"pin": "0842631"})
                check(st == 200 and r.get("ok") is True, "a PIN with hub_btstack running: 200", "%s %s" % (st, r))
                st, r = hub.http_post("/api/bt-pairing-pin", {"cancel": True})
                check(st == 200 and r.get("cancelled") is True, "cancel: 200", "%s %s" % (st, r))
                time.sleep(0.3)
                sent = sent_since(log_path, mark)
                check(sent == ["pin_reply 0842631", "pin_cancel"], "pin_reply keeps the leading zero, then pin_cancel", str(sent))

            print("[-] device command PS3:<hex>")
            # the key pipe, as test_bt_api_checks.py reads it (the mock root may hold it too)
            mock_pipe = os.path.join(hub.mock_dir, "bthid_input")
            fd = open_pipe_reader(PIPE)
            mock_fd = open_pipe_reader(mock_pipe)
            if fd is None:
                print("  SKIP the key pipe is in use: PS3 key lines not checked")
            else:
                try:
                    read_pipe(fd, 0.3)  # the daemon's own release at start
                    lines = ""
                    for cmd, action in (("PS", "press"), ("PS", "release"), ("Triangle", None), ("Bad", None)):
                        body = {"deviceId": "3", "command": cmd}
                        if action:
                            body["action"] = action
                        hub.http_post("/api/ir-send", body)
                        lines += read_pipe(fd, 0.4) + (read_pipe(mock_fd, 0.05) if mock_fd is not None else "")
                    check(("TARGET %s\nPS3_DOWN 43\n" % PS3) in lines, "PS press: PS3_DOWN 43", repr(lines))
                    check("RELEASE\n" in lines, "PS release: RELEASE", repr(lines))
                    check("PS3 5C\n" in lines, "Triangle (ps3:5c) tap: PS3 5C", repr(lines))
                    check("PS3 5C\n" in lines and "KEY Bad\n" in lines, "PS3:5cx is no code: sent by name", repr(lines))
                finally:
                    os.close(fd)
                    os.unlink(PIPE)
                    if mock_fd is not None:
                        os.close(mock_fd)
                        os.unlink(mock_pipe)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
    if failed:
        print("[-] test_bt_pairing_pin_api: %d failed" % failed)
        sys.exit(1)
    print("[+] test_bt_pairing_pin_api: profile, PIN route and PS3 key lines")


if __name__ == "__main__":
    main()
