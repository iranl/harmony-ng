#!/usr/bin/env python3
"""
POST /api/bt-forget-host {"addr": ...} removes a paired Bluetooth host (there was no way to remove
one short of deleting the key store):
- the address is checked (400 for anything but "XX:XX:XX:XX:XX:XX")
- nothing changes while hub_btstack is not running (503): the link key lives there
- the devices linked to the host lose their link in devices.json (also a legacy "btHost"), other
  devices keep theirs
- the default host file (bthid_host_target) goes when it names the host, stays otherwise
- hub_btstack gets "host_forget <addr>"
- POST /api/bt-link-device answers 404 for an unknown device and 400 without one (hub_btstack drops
  a waiting link only then; it read {"ok":false} as "no such device" also when the save failed)
- admin only: a user account gets 403
- 500 when hub_btstack did not take the command (a test hook plays a hub_btstack that is gone)
The daemon runs with --dry-run on its own mock root (its hub_btstack commands are printed). A short
lived process named hub_btstack stands in for a running hub_btstack (hub_daemon finds it in /proc):
run it under flock /tmp/hng_suite.lock so that no other test sees it.
"""

import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

PORT = 9242
PORT_AUTH = 9243
PORT_GONE = 9245
BOX = "00:11:22:33:44:01"  # test addresses, not real devices
TV = "00:11:22:33:44:02"
OLD = "00:11:22:33:44:03"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def config_dir():
    d = tempfile.mkdtemp(prefix="bt_forget_")
    devices = json.load(open(os.path.join(SAMPLE_CONFIG, "devices.json")))
    devices["devices"].append({"id": "3", "name": "Box", "type": "BT", "bdaddr": BOX.lower(),
                               "commands": [{"name": "Home", "raw": "HID:0C:0223"}]})
    devices["devices"].append({"id": "4", "name": "TV Box", "type": "BT", "bdaddr": TV,
                               "commands": [{"name": "Home", "raw": "HID:0C:0223"}]})
    devices["devices"].append({"id": "5", "name": "Old Box", "type": "BT", "btHost": OLD,
                               "commands": [{"name": "Home", "raw": "HID:0C:0223"}]})
    json.dump(devices, open(os.path.join(d, "devices.json"), "w"))
    for name in ("activities.json", "settings.json", "remote_slot1_map.json"):
        src = os.path.join(SAMPLE_CONFIG, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, name))
    return d


def device_addrs(hub):
    devs = json.load(open(os.path.join(hub.data_dir, "config", "devices.json")))["devices"]
    return {str(x["id"]): x.get("bdaddr") or x.get("btHost") for x in devs}


class FakeBtstack:
    """A process whose name is hub_btstack, as hub_daemon looks for it in /proc"""

    def __enter__(self):
        self.dir = tempfile.mkdtemp(prefix="bt_forget_proc_")
        exe = os.path.join(self.dir, "hub_btstack")
        shutil.copy(shutil.which("sleep"), exe)
        self.proc = subprocess.Popen([exe, "60"])
        time.sleep(2.2)  # hub_daemon keeps its answer for 2 s
        return self

    def __exit__(self, *exc):
        self.proc.kill()
        self.proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)
        time.sleep(2.2)
        return False


def main():
    cfg = config_dir()
    log = tempfile.NamedTemporaryFile(prefix="bt_forget_log_", delete=False)
    log.close()
    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log.name,
                        extra_data_files={"bthid_host_target": "bdaddr=%s\n" % BOX}) as hub:
            host_file = os.path.join(hub.data_dir, "bthid_host_target")
            print("[-] addresses")
            for bad in ("", "001122334401", "0:1:2:3:4:5", BOX + "\nscan", "00:00:00:00:00:00"):
                st, _ = hub.http_post("/api/bt-forget-host", {"addr": bad})
                check(st == 400, "%r: 400" % bad, str(st))

            print("[-] hub_btstack not running")
            st, r = hub.http_post("/api/bt-forget-host", {"addr": BOX})
            check(st == 503, "503 while Bluetooth is off", "%s %s" % (st, r))
            check(device_addrs(hub).get("3") == BOX.lower() and os.path.exists(host_file),
                  "and nothing was changed", str(device_addrs(hub)))

            with FakeBtstack():
                print("[-] forget a host")
                st, r = hub.http_post("/api/bt-forget-host", {"addr": BOX.lower()})
                check(st == 200 and r.get("ok") is True and r.get("devices_cleared") == 1 and r.get("default_host_cleared") is True,
                      "200, one device and the default host cleared", "%s %s" % (st, r))
                addrs = device_addrs(hub)
                check(not addrs.get("3") and addrs.get("4") == TV,
                      "the device linked to it has no host any more, the other keeps its own", str(addrs))
                check(not os.path.exists(host_file), "the default host file naming it is gone")
                st2, inv = hub.http_get("/api/inventory")
                inv_devs = {str(d.get("id")): d for d in (inv.get("devices", []) if isinstance(inv, dict) else [])}
                check("3" in inv_devs and not inv_devs["3"].get("bdaddr"), "hub_daemon's own copy has no link either", str(inv_devs.get("3")))

                print("[-] forget another host")
                with open(host_file, "w") as f:
                    f.write("bdaddr=%s\n" % BOX)
                st, r = hub.http_post("/api/bt-forget-host", {"bdaddr": TV})
                check(st == 200 and r.get("devices_cleared") == 1 and r.get("default_host_cleared") is False,
                      "\"bdaddr\" is taken too; a default host file naming another host stays", "%s %s" % (st, r))
                check(os.path.exists(host_file), "the file is still there")
                st, r = hub.http_post("/api/bt-forget-host", {"addr": OLD})
                check(st == 200 and r.get("devices_cleared") == 1 and not device_addrs(hub).get("5"),
                      "a device linked with the legacy btHost key is cleared too", "%s %s %s" % (st, r, device_addrs(hub)))
                st, r = hub.http_post("/api/bt-forget-host", {"addr": TV})
                check(st == 200 and r.get("devices_cleared") == 0, "a host no device uses: 200, nothing to clear", "%s %s" % (st, r))
                time.sleep(0.3)
                out = open(log.name, errors="replace").read()
                check(("host_forget %s" % BOX) in out and ("host_forget %s" % TV) in out,
                      "hub_btstack is told to forget each host, in upper case", out[-400:])

                print("[-] POST /api/bt-link-device answers")
                st, r = hub.http_post("/api/bt-link-device", {"deviceId": "99", "bdaddr": BOX})
                check(st == 404, "an unknown device: 404 (it was 200 {\"ok\": false})", "%s %s" % (st, r))
                st, r = hub.http_post("/api/bt-link-device", {"bdaddr": BOX})
                check(st == 400, "no device id: 400", "%s %s" % (st, r))
                st, r = hub.http_post("/api/bt-link-device", {"deviceId": "3", "bdaddr": BOX})
                check(st == 200 and r.get("ok") is True and device_addrs(hub).get("3") == BOX, "a known device: 200, linked", "%s %s" % (st, r))

                print("[-] hub_btstack does not take the command")
                with HubHarness(port=PORT_GONE, config_dir=cfg, env={"HARMONY_TEST_BTSTACK_GONE": "1"}) as ghub:
                    st, r = ghub.http_post("/api/bt-forget-host", {"addr": BOX})
                    check(st == 500 and "try again" in str(r), "500, and the answer says to send it again", "%s %s" % (st, r))
                    st, r = ghub.http_post("/api/bt-forget-host", {"addr": BOX})
                    check(st == 500 and not device_addrs(ghub).get("3"), "the devices were cleared already; it stays 500 while hub_btstack is gone", "%s %s" % (st, r))

                print("[-] accounts")
                users = {"auth_enabled": True, "users": [
                    {"username": "admin", "password": "secret123", "role": "admin"},
                    {"username": "guest", "password": "guestpass", "role": "user"}]}
                with HubHarness(port=PORT_AUTH, host="127.0.0.2", config_dir=cfg,
                                extra_config_files={"users.json": users}) as ahub:
                    def basic(u, p):
                        return {"Authorization": "Basic " + base64.b64encode(("%s:%s" % (u, p)).encode()).decode()}
                    st, _ = ahub.http_post("/api/bt-forget-host", {"addr": BOX}, headers=basic("guest", "guestpass"))
                    check(st == 403, "a user account: 403", str(st))
                    st, _ = ahub.http_post("/api/bt-forget-host", {"addr": BOX}, headers=basic("admin", "secret123"))
                    check(st == 200, "an admin: 200", str(st))
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        os.unlink(log.name)
    if failed:
        print("[-] test_bt_forget_host: %d failed" % failed)
        sys.exit(1)
    print("[+] test_bt_forget_host: a paired Bluetooth host can be removed")


if __name__ == "__main__":
    main()
