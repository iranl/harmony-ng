#!/usr/bin/env python3
"""
Combined devices (payload/source/net_device.h): one device, a transport per command.

- device-save keeps the settings of each listed transport (IR controlPort, plugin + config, the
  "http" and "tcp" connections) and drops those of transports it does not list;
- every command keeps the fields of its own transport: IR port/repeats only on IR commands,
  HTTP:/TCP: keyCodes and request fields only on HTTP/TCP commands; an unknown transport falls
  back to the device's first one;
- an HTTP command (and the connection test) uses the "http" connection, a TCP command the "tcp"
  one, on two ports of one host; the command's own headers win over the connection's;
- httpPassword inside "http" is never returned (hasHttpPassword) and a save without it keeps it;
- in dry run, IR and plugin commands go to their own senders;
- command-save and commands-batch set the transport; a plain HTTP device turned into a combined
  one keeps its connection (moved into "http") and its commands (marked "http");
- a plain HTTP device saves as before (no "transport" on its commands).
"""

import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402
from test_net_device_command import REQUESTS, TcpRecorder, http_server  # noqa: E402

PORT = 8976


def config_dir():
    d = tempfile.mkdtemp(prefix="combined_dev_")
    for name in ("devices.json", "activities.json", "settings.json"):
        shutil.copy(os.path.join(SAMPLE_CONFIG, name), os.path.join(d, name))
    return d


PRONTO = ("0000 0067 0000 000d 0060 0018 0030 0018 0018 0018 0030 0018 0018 0018 0030 0018 0018 0018 "
          "0018 0018 0030 0018 0018 0018 0018 0018 0018 0018 0018 03a0")


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + str(detail)))
        if not cond:
            failures.append(name)

    cfg = config_dir()
    tmp = tempfile.mkdtemp(prefix="combined_log_")
    log = os.path.join(tmp, "hub.log")
    web = http_server()
    tcp = TcpRecorder()
    wport = web.server_address[1]
    ports = "%d,%d" % (wport, tcp.port)
    pronto = PRONTO

    def log_text():
        time.sleep(0.3)
        return open(log, errors="replace").read()

    try:
        with HubHarness(port=PORT, config_dir=cfg, log_path=log, env={"HARMONY_TEST_NET_LOOPBACK_PORTS": ports}) as hub:
            def dev_by_id(dev_id):
                st, r = hub.http_get("/api/devices")
                return next((d for d in r.get("devices", []) if d.get("id") == dev_id), None)

            dev = {"id": "60", "name": "Living Receiver", "type": "Combined",
                   "transports": ["tcp", "http", "ir", "plugin"],
                   "controlPort": 2, "plugin": "samsung-tv", "config": {"host": "10.0.0.9"},
                   "mqttTopic": "should/be/dropped",
                   "http": {"netHost": "127.0.0.1", "netPort": wport, "httpAuthType": "basic",
                            "httpUser": "u", "httpPassword": "p",
                            "httpHeaders": [{"name": "X-Dev", "value": "d"}, {"name": "SOAPACTION", "value": "dev"}]},
                   "tcp": {"netHost": "127.0.0.1", "netPort": tcp.port},
                   "commands": [
                       {"name": "PowerOn", "transport": "tcp", "tcpPayload": "PWON\\r", "port": 3, "repeats": 5},
                       {"name": "Play", "transport": "http", "httpMethod": "POST", "httpPath": "/ctl",
                        "httpBody": "<x/>", "httpHeaders": [{"name": "SOAPACTION", "value": "cmd#Play"}]},
                       {"name": "HDMI 1", "transport": "ir", "keyCode": pronto, "port": 4},
                       {"name": "Home", "transport": "plugin", "keyCode": "PLUGIN:samsung-tv:KEY_HOME"},
                       {"name": "Odd", "transport": "mqtt", "tcpPayload": "X"}]}
            st, r = hub.http_post("/api/device-save", dev)
            check(st == 200 and r.get("ok") is not False, "combined device saved", (st, r))
            d = dev_by_id("60") or {}
            check(d.get("type") == "Combined" and d.get("transports") == ["tcp", "http", "ir", "plugin"],
                  "type and transports kept", (d.get("type"), d.get("transports")))
            check(d.get("controlPort") == 2 and d.get("plugin") == "samsung-tv" and
                  d.get("config", {}).get("host") == "10.0.0.9", "IR port and plugin settings kept", d)
            check("mqttTopic" not in d and "netHost" not in d, "settings of unlisted transports dropped", d)
            h = d.get("http", {})
            check(h.get("netPort") == wport and h.get("httpPassword") == "" and h.get("hasHttpPassword") is True,
                  "http connection kept, password hidden", h)
            check(d.get("tcp", {}).get("netPort") == tcp.port, "tcp connection kept", d.get("tcp"))
            cmds = {c["name"]: c for c in d.get("commands", [])}
            p_on = cmds.get("PowerOn", {})
            check(p_on.get("transport") == "tcp" and p_on.get("keyCode") == "TCP:PowerOn" and
                  "port" not in p_on and "repeats" not in p_on, "TCP command: TCP keyCode, no IR fields", p_on)
            play = cmds.get("Play", {})
            check(play.get("keyCode") == "HTTP:Play" and play.get("httpHeaders") == [{"name": "SOAPACTION", "value": "cmd#Play"}],
                  "HTTP command keeps its own headers", play)
            hdmi = cmds.get("HDMI 1", {})
            check(hdmi.get("transport") == "ir" and hdmi.get("port") == 4 and hdmi.get("keyCode") == pronto,
                  "IR command keeps port and code", hdmi)
            check(cmds.get("Home", {}).get("transport") == "plugin", "plugin command", cmds.get("Home"))
            odd = cmds.get("Odd", {})
            check(odd.get("transport") == "tcp" and odd.get("keyCode") == "TCP:Odd", "unlisted transport falls back to the first", odd)

            # password kept on a save without it
            st, r = hub.http_post("/api/device-save", {"id": "60", "http": {"netHost": "127.0.0.1", "netPort": wport}})
            check(st == 200, "save without the password", (st, r))

            REQUESTS.clear()
            st, r = hub.http_post("/api/device-net-test", {"id": "60", "command": "Play"}, timeout=15)
            check(st == 200 and r.get("ok") is True, "HTTP command test", (st, r))
            req = REQUESTS[-1] if REQUESTS else {}
            hh = req.get("headers", {})
            check(req.get("method") == "POST" and req.get("path") == "/ctl" and req.get("body") == b"<x/>",
                  "HTTP command sent to the http connection", req)
            check(hh.get("SOAPACTION") == "cmd#Play" and hh.get("X-Dev") == "d", "command header wins, device header kept", hh)
            check(hh.get("Authorization") == "Basic dTpw", "stored password used", hh.get("Authorization"))

            st, r = hub.http_post("/api/device-net-test", {"id": "60", "command": "PowerOn"}, timeout=15)
            check(st == 200 and r.get("ok") is True and tcp.got and tcp.got[-1] == b"PWON\r",
                  "TCP command sent to the tcp connection", (r, tcp.got[-1:]))
            st, r = hub.http_post("/api/device-net-test", {"id": "60", "transport": "http"}, timeout=15)
            check(st == 200 and r.get("ok") is True, "connection test of the http connection", (st, r))
            st, r = hub.http_post("/api/device-net-test", {"id": "60"}, timeout=15)
            check(st == 400, "connection test needs a transport on a combined device", st)
            st, r = hub.http_post("/api/device-net-test", {"id": "60", "command": "HDMI 1"}, timeout=15)
            check(st == 400, "IR command is not a net test", st)

            # dry run: each command over its own sender
            for name in ("HDMI 1", "Home", "Play", "PowerOn"):
                st, r = hub.http_post("/api/ir-send", {"deviceId": "60", "command": name}, timeout=5)
                check(st == 200, "ir-send " + name, (st, r))
            text = log_text()
            check("port_mask 0x04" in text, "IR command over IR with its own port")
            check("Plugin command simulated: plugin=samsung-tv dev=60" in text and "cmd=KEY_HOME" in text,
                  "plugin command to the plugin")
            check("[DRY-RUN] HTTP POST http://127.0.0.1:%d/ctl" % wport in text, "HTTP command in dry run")
            check("[DRY-RUN] TCP 127.0.0.1:%d" % tcp.port in text, "TCP command in dry run")

            # command-save and commands-batch
            st, r = hub.http_post("/api/command-save", {"deviceId": "60", "name": "VolUp", "raw": "",
                                                        "transport": "tcp", "tcpPayload": "MVUP\\r"})
            st, r = hub.http_post("/api/command-save", {"deviceId": "60", "name": "Back", "raw": pronto,
                                                        "transport": "ir", "port": 1, "repeats": 2})
            st, r = hub.http_post("/api/commands-batch", {"deviceId": "60", "commands": [
                {"name": "Status", "raw": "", "transport": "http", "httpPath": "/status"},
                {"name": "Menu", "raw": "PLUGIN:samsung-tv:KEY_MENU"}]})
            cmds = {c["name"]: c for c in (dev_by_id("60") or {}).get("commands", [])}
            v = cmds.get("VolUp", {})
            check(v.get("transport") == "tcp" and v.get("tcpPayload") == "MVUP\\r", "command-save TCP command", v)
            b = cmds.get("Back", {})
            check(b.get("transport") == "ir" and b.get("port") == 1 and b.get("repeats") == 2, "command-save IR command", b)
            s = cmds.get("Status", {})
            check(s.get("transport") == "http" and s.get("httpPath") == "/status", "batch HTTP command", s)
            m = cmds.get("Menu", {})
            check(m.get("transport") == "plugin" and "port" not in m, "batch PLUGIN: code goes to the plugin", m)

            # a plain HTTP device: unchanged, then turned into a combined one
            plain = {"id": "61", "name": "Lamp", "type": "HTTP", "netHost": "127.0.0.1", "netPort": wport,
                     "httpAuthType": "bearer", "httpToken": "t0k",
                     "commands": [{"name": "On", "httpPath": "/on",
                                   "httpHeaders": [{"name": "X-Cmd", "value": "1"}]}]}
            hub.http_post("/api/device-save", plain)
            d = dev_by_id("61") or {}
            on = (d.get("commands") or [{}])[0]
            check(d.get("type") == "HTTP" and "transport" not in on and "transports" not in d,
                  "plain HTTP device saves as before", d)
            check(on.get("httpHeaders") == [{"name": "X-Cmd", "value": "1"}], "plain HTTP command keeps its headers", on)
            st, r = hub.http_post("/api/device-save", {"id": "61", "type": "Combined"})
            d = dev_by_id("61") or {}
            on = (d.get("commands") or [{}])[0]
            check(d.get("transports") == ["http"] and d.get("http", {}).get("netPort") == wport and
                  d.get("http", {}).get("hasHttpToken") is True and "netHost" not in d,
                  "converted: connection moved into http", d)
            check(on.get("transport") == "http" and on.get("keyCode") == "HTTP:On", "converted: command marked http", on)
            REQUESTS.clear()
            st, r = hub.http_post("/api/device-net-test", {"id": "61", "command": "On"}, timeout=15)
            req = REQUESTS[-1] if REQUESTS else {}
            check(r.get("ok") is True and req.get("headers", {}).get("Authorization") == "Bearer t0k",
                  "converted: token still sent", (r, req.get("headers")))

            # a new combined device in one JSON request (the web UI's device templates)
            st, r = hub.http_post("/api/new-device", {
                "name": "Kodi", "manufacturer": "Kodi", "model": "21", "type": "Combined", "transports": ["http", "bt"],
                "http": {"netHost": "127.0.0.1", "netPort": wport, "httpAuthType": "basic",
                         "httpUser": "kodi", "httpPassword": "pw"},
                "commands": [{"name": "Home", "transport": "http", "httpMethod": "POST", "httpPath": "/jsonrpc",
                              "httpBody": '{"jsonrpc":"2.0","id":1,"method":"Input.Home"}'}]})
            new_id = r.get("deviceId") if isinstance(r, dict) else None
            d = dev_by_id(new_id) or {}
            names = {c["name"]: c for c in d.get("commands", [])}
            check(st == 200 and d.get("transports") == ["http", "bt"] and d.get("http", {}).get("hasHttpPassword") is True,
                  "new combined device from one JSON request", (st, r, d))
            check(names.get("Home", {}).get("keyCode") == "HTTP:Home" and names.get("Connect", {}).get("transport") == "bt",
                  "its HTTP command, and Connect added for bt", names)
            REQUESTS.clear()
            st, r = hub.http_post("/api/device-net-test", {"id": new_id, "command": "Home"}, timeout=15)
            req = REQUESTS[-1] if REQUESTS else {}
            check(r.get("ok") is True and req.get("headers", {}).get("Authorization") == "Basic a29kaTpwdw==",
                  "new combined device sends with its password", (r, req.get("headers")))

            st, _ = hub.http_post("/api/device-save", {"id": "60", "transports": ["fax"]})
            check(st == 400, "400 for an unknown transport", st)
            st, _ = hub.http_post("/api/device-save", {"id": "60", "http": {"netPort": 70000}})
            check(st == 400, "400 for a bad port in http", st)
    finally:
        web.shutdown()
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print("FAILED: %d check(s): %s" % (len(failures), ", ".join(failures)))
        if os.path.exists(log):
            print(open(log, errors="replace").read()[-3000:])
        sys.exit(1)
    shutil.rmtree(tmp, ignore_errors=True)
    print("all combined device checks passed")


if __name__ == "__main__":
    main()
