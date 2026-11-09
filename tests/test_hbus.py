#!/usr/bin/env python3
"""
Logitech protocol shim (spec 6, Appendix B) against hub_daemon_test with legacy_enabled: the TCP
probe, the provisioning POST (with the "id " key), /description, the WebSocket envelope, config,
getCurrentActivity, runactivity with the 100 / 200 / startActivityFinished sequence, holdAction,
changeChannel, statedigest, discoveryinfo, sync, unknown command 404, the second client, the
separation from the 8089 clients, and the setting toggling the listener.
"""

import copy
import json
import os
import socket
import sys
import time
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, WsClient, SAMPLE_CONFIG  # noqa: E402

PORT = 8993
LEGACY_PORT = 18088
REMOTE_ID = 1234567
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def settings(enabled):
    with open(os.path.join(SAMPLE_CONFIG, "settings.json")) as f:
        s = json.load(f)
    s["legacy_enabled"] = enabled
    return s


def port_open(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=1.0)
        s.close()
        return True
    except OSError:
        return False


def provision(hub, key="id "):
    body = json.dumps({key: 1, "cmd": "setup.account?getProvisionInfo", "params": {}})
    conn = socket.create_connection(("127.0.0.1", LEGACY_PORT), timeout=3.0)
    req = ("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://sl.dhg.myharmony.com\r\n"
           "Content-Type: application/json\r\nAccept: application/json\r\nContent-Length: %d\r\n\r\n%s"
           % (len(body), body))
    conn.sendall(req.encode())
    data = b""
    while True:
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    conn.close()
    head, _, payload = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ")[1])
    return status, head.decode(), json.loads(payload.decode() or "{}")


def http_get(path):
    conn = socket.create_connection(("127.0.0.1", LEGACY_PORT), timeout=3.0)
    conn.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n" % path).encode())
    data = b""
    while True:
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    conn.close()
    head, _, payload = data.partition(b"\r\n\r\n")
    return int(head.split(b" ")[1]), head.decode(), payload.decode()


def legacy_ws(remote_id):
    return WsClient("127.0.0.1", LEGACY_PORT, path="/?domain=svcs.myharmony.com&hubId=%s" % remote_id, timeout=5.0)


def hbus(ws, cmd, params=None, remote_id=REMOTE_ID):
    rid = str(uuid.uuid4())
    ws.send_json({"hubId": str(remote_id), "timeout": 5,
                  "hbus": {"cmd": cmd, "id": rid, "params": params or {"verb": "get", "format": "json"}}})
    return rid


def recv_until(ws, pred, timeout=5.0):
    """All frames until pred(frame) is true; (match, frames_before)"""
    before = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            m = ws.recv_json(timeout=max(0.05, deadline - time.monotonic()))
        except Exception:
            break
        if m is None:
            break
        if pred(m):
            return m, before
        before.append(m)
    return None, before


def reply_for(ws, rid, timeout=5.0):
    return recv_until(ws, lambda m: m.get("id") == rid, timeout)


def main():
    global failed
    print("[-] 6.1 legacy_enabled off: port %d closed" % LEGACY_PORT)
    with HubHarness(port=PORT, args=["--legacy-port", str(LEGACY_PORT), "--ssdp-port", "0"],
                    extra_config_files={"settings.json": settings(False)}) as hub:
        check(not port_open(LEGACY_PORT), "nothing listens on the legacy port")
        st, status = hub.http_get("/api/status")
        check(status.get("legacy_enabled") is False and "legacy" in status.get("capabilities", []),
              "status legacy_enabled false, capability legacy", str(status.get("capabilities")))

    print("[-] 6.2 provisioning and description")
    prov_file = {"remoteId": REMOTE_ID, "accountId": 42}
    with HubHarness(port=PORT, args=["--legacy-port", str(LEGACY_PORT), "--ssdp-port", "0"],
                    extra_config_files={"settings.json": settings(True)},
                    extra_root_files={"luaworks/provision/preferences": prov_file}) as hub:
        check(port_open(LEGACY_PORT), "listener open with the setting on")
        # the TCP probe: connect, no data, close; the next request must still be served
        s = socket.create_connection(("127.0.0.1", LEGACY_PORT), timeout=1.0)
        s.close()
        st, head, prov = provision(hub)
        check(st == 200 and "application/json" in head, "POST / answers 200 JSON", head.splitlines()[0] if head else "")
        d = prov.get("data", {})
        check(prov.get("id") == 1 and prov.get("code") == 200 and prov.get("msg") == "OK", "id echoed, code 200 as a number", str(prov)[:120])
        check(d.get("activeRemoteId") == str(REMOTE_ID), "activeRemoteId from the stock provisioning file", str(d.get("activeRemoteId")))
        check(d.get("discoveryServer", "").startswith("https://svcs.myharmony.com/"), "discoveryServer host svcs.myharmony.com", d.get("discoveryServer"))
        check(d.get("hubName") == "Test Hub" and d.get("accountId") == "0" and d.get("email") == "" and "ethernetStatus" not in d,
              "hubName, accountId, email, no ethernetStatus", str(d))
        check(d.get("hubSwVersion") == hub.http_get("/api/status")[1].get("version"), "hubSwVersion = version")
        st2, _, prov2 = provision(hub, key="id")
        check(st2 == 200 and prov2.get("data", {}).get("activeRemoteId") == str(REMOTE_ID), "second POST with a plain id key")
        st, head, xml = http_get("/description")
        check(st == 200 and "text/xml" in head, "GET /description 200 xml", head.splitlines()[0] if head else "")
        check("<deviceType>urn:myharmony-com:device:harmony:1</deviceType>" in xml and "<manufacturer>Logitech</manufacturer>" in xml
              and "<friendlyName>Test Hub</friendlyName>" in xml and "<modelName>Harmony Hub</modelName>" in xml
              and "<UDN>uuid:" in xml and "<serialNumber>" in xml, "description fields", xml[:200])
        st, _, _ = http_get("/other")
        check(st == 404, "GET /other is 404", str(st))
        st, _, _ = http_get("/api/status")
        check(st == 404, "the /api router is not reachable on the legacy port", str(st))

        print("[-] 6.3 WebSocket envelope and digest on connect")
        ws = legacy_ws(REMOTE_ID)
        first, _ = recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify", 3.0)
        check(first is not None, "digest notify right after the upgrade")
        dg = (first or {}).get("data", {})
        check(isinstance(dg.get("configVersion"), int) and dg.get("syncStatus") == 0 and dg.get("activityId") == "-1"
              and dg.get("activityStatus") == 0 and dg.get("runningActivityList") == "" and isinstance(dg.get("hubSwVersion"), str),
              "digest fields", str(dg)[:200])
        cfg_version = dg.get("configVersion")

        print("[-] 6.4 commands")
        rid = hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?config")
        r, _ = reply_for(ws, rid)
        check(r is not None and r.get("code") == 200 and r.get("cmd", "").endswith("?config"), "config reply with the request id and code 200")
        data = (r or {}).get("data", {})
        acts = data.get("activity", [])
        ids = [a.get("id") for a in acts]
        check(ids == ["10", "11", "-1"], "activities 10, 11 and PowerOff -1 last", str(ids))
        check(all(int(a["id"]) is not None for a in acts) and acts[-1].get("label") == "PowerOff" and acts[-1].get("type") == "PowerOff",
              "int() ids, PowerOff label and type")
        devs = data.get("device", [])
        check([x.get("id") for x in devs] == ["1", "2"] and devs[0].get("label") == "Living Room TV", "devices with numeric ids and labels")
        fn = devs[0]["controlGroup"][0]["function"][0]
        action = json.loads(fn.get("action", "{}"))
        check(action.get("command") == fn.get("name") and action.get("type") == "IRCommand" and action.get("deviceId") == "1",
              "function.action is a JSON string with command, type and deviceId", fn.get("action"))
        act10 = acts[0]
        act_cmds = set(f["name"] for g in act10["controlGroup"] for f in g["function"])
        check({"PowerOn", "VolumeUp", "InputHdmi1"} <= act_cmds and "1" in act10.get("fixit", {}), "activity control groups and fixit", str(act_cmds))
        check(data.get("global", {}).get("timeStampHash", "").endswith(";%d" % REMOTE_ID), "global.timeStampHash ends with the remote id",
              data.get("global", {}).get("timeStampHash"))

        rid = hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?getCurrentActivity")
        r, _ = reply_for(ws, rid)
        check(r is not None and r.get("code") == 200 and r.get("data", {}).get("result") == "-1", "getCurrentActivity -1 while idle", str(r))

        rid = hbus(ws, "connect.discoveryinfo?get")
        r, _ = reply_for(ws, rid)
        di = (r or {}).get("data", {})
        check(di.get("friendlyName") == "Test Hub" and di.get("remoteId") == str(REMOTE_ID) and di.get("protocolVersion") == "1.0"
              and di.get("hubId") and di.get("current_fw_version"), "discoveryinfo", str(di))

        rid = hbus(ws, "setup.sync?None")
        r, _ = reply_for(ws, rid)
        check(r is not None and r.get("code") == 200 and r.get("data") == {}, "setup.sync?None 200 {}")

        rid = hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?nosuch")
        r, _ = reply_for(ws, rid)
        check(r is not None and r.get("code") == 404 and r.get("msg") == "Not Found" and r.get("data") == {}, "unknown command 404", str(r))

        print("[-] 6.7 runactivity sequencing")
        ws2 = legacy_ws(REMOTE_ID)
        recv_until(ws2, lambda m: m.get("type") == "connect.stateDigest?notify", 3.0)
        plain = hub.ws()  # an 8089 client: must never see legacy frames
        plain.wait_for("snapshot", 5)
        rid = hbus(ws, "harmony.activityengine?runactivity", {"async": "true", "timestamp": 0, "activityId": "10"})
        r, before = reply_for(ws, rid, 3.0)
        check(r is not None and r.get("code") == 200 and r.get("cmd") == "harmony.activityengine?runactivity" and r.get("data") == {},
              "runactivity acknowledged with code 200", str(r))
        fin, frames = recv_until(ws, lambda m: m.get("type") == "harmony.engine?startActivityFinished", 15.0)
        check(fin is not None and fin.get("data", {}).get("activityId") == "10" and fin["data"].get("errorCode") == "200",
              "startActivityFinished for 10", str(fin))
        notifies = [f for f in frames if f.get("type") == "connect.stateDigest?notify"]
        check(any(n["data"].get("activityStatus") == 1 and n["data"].get("activityId") == "10" for n in notifies),
              "digest notify activityStatus 1 with the target id", str([n["data"].get("activityStatus") for n in notifies]))
        progress = [f for f in frames if f.get("cmd") == "harmony.engine?startActivity" and f.get("code") == 100]
        check(len(progress) >= 1 and all(f.get("id") == rid and "done" in f["data"] and "total" in f["data"] for f in progress),
              "startActivity code 100 progress frames with the request id (%d)" % len(progress))
        done = [f for f in frames if f.get("cmd") == "harmony.engine?startActivity" and f.get("code") == 200]
        check(len(done) == 1 and done[0].get("id") == rid and done[0].get("data") == {}, "exactly one startActivity code 200 with the request id")
        last, _ = recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify" and m["data"].get("activityStatus") == 2, 3.0)
        check(last is not None and last["data"].get("activityId") == "10" and last["data"].get("runningActivityList") == "10",
              "final digest notify activityStatus 2", str(last))
        # the second client: notifies and finished, but no startActivity frames
        f2 = []
        while True:
            m, before2 = recv_until(ws2, lambda m: m.get("type") == "connect.stateDigest?notify" and m["data"].get("activityStatus") == 2, 3.0)
            f2 += before2
            if m is not None:
                f2.append(m)
            break
        check(any(f.get("type") == "harmony.engine?startActivityFinished" for f in f2) and not any(f.get("cmd") for f in f2),
              "second client gets the notifications and no startActivity frames", str([f.get("type") or f.get("cmd") for f in f2]))
        check(not any(f.get("type") in ("activity_state", "activity_progress") for f in frames + f2), "no 8089 event reached a legacy client")
        ev, pframes = plain.wait_for("activity_state", 5, lambda m: m.get("state") == 2)
        check(ev is not None and not any(("hbus" in f or f.get("type", "").startswith("connect.")) for f in pframes),
              "8089 client gets its activity_state and no legacy frame")

        rid = hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?getCurrentActivity")
        r, _ = reply_for(ws, rid)
        check(r is not None and r.get("data", {}).get("result") == "10", "getCurrentActivity 10")
        rid = hbus(ws, "vnd.logitech.connect/vnd.logitech.statedigest?get")
        r, _ = reply_for(ws, rid)
        dg = (r or {}).get("data", {})
        check(r is not None and r.get("code") == 200 and dg.get("activityId") == "10" and dg.get("activityStatus") == 2
              and dg.get("runningActivityList") == "10" and dg.get("configVersion") == cfg_version, "statedigest?get while running", str(dg)[:160])

        rid = hbus(ws, "harmony.activityengine?runactivity", {"async": "true", "timestamp": 0, "activityId": "999"})
        r, _ = reply_for(ws, rid, 3.0)
        check(r is not None and r.get("code") == 500 and r.get("msg") == "Unknown activity", "unknown activity 500", str(r))

        print("[-] holdAction and changeChannel")
        act = '{"command":: "VolumeUp","type":: "IRCommand","deviceId":: "1"}'
        rid_p = hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?holdAction",
                     {"status": "press", "timestamp": "0", "verb": "render", "action": act})
        rid_r = hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?holdAction",
                     {"status": "release", "timestamp": "0", "verb": "render", "action": act})
        m, _ = recv_until(ws, lambda m: m.get("id") in (rid_p, rid_r), 0.8)
        check(m is None, "holdAction press and release: no reply", str(m))
        rid = hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?holdAction",
                   {"status": "press", "timestamp": "0", "verb": "render", "action": "not json"})
        r, _ = reply_for(ws, rid, 1.0)
        check(r is not None and r.get("code") == 500, "holdAction with a bad action: code 500 reply", str(r))
        rid = hbus(ws, "harmony.engine?changeChannel", {"timestamp": 0, "channel": "12"})
        r, _ = reply_for(ws, rid, 1.0)
        check(r is not None and r.get("code") == 500, "changeChannel without a channel device: 500", str(r))

        print("[-] power off")
        rid = hbus(ws, "harmony.activityengine?runactivity", {"async": "true", "timestamp": 0, "activityId": "-1"})
        r, _ = reply_for(ws, rid, 3.0)
        check(r is not None and r.get("code") == 200, "runactivity -1 acknowledged")
        fin, frames = recv_until(ws, lambda m: m.get("type") == "harmony.engine?startActivityFinished", 15.0)
        check(fin is not None and fin["data"].get("activityId") == "-1", "startActivityFinished -1", str(fin))
        check(any(f.get("type") == "connect.stateDigest?notify" and f["data"].get("activityStatus") == 0 and f["data"].get("activityId") == "-1"
                  for f in frames), "digest notify activityStatus 0 for power off")
        check(any(f.get("cmd") == "harmony.engine?startActivity" and f.get("code") == 200 and f.get("id") == rid for f in frames),
              "startActivity 200 completes the power off")
        last, _ = recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify" and m["data"].get("activityStatus") == 0, 3.0)
        check(last is not None, "final digest after power off")

        print("[-] a switch from the REST API: notifications only")
        hub.http_post("/api/activity", {"activity": "11"})
        fin, frames = recv_until(ws, lambda m: m.get("type") == "harmony.engine?startActivityFinished", 15.0)
        check(fin is not None and fin["data"].get("activityId") == "11", "finished for 11 after a REST switch")
        check(not any(f.get("cmd") for f in frames), "no startActivity frames without a legacy request", str([f.get("cmd") for f in frames if f.get("cmd")]))
        recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify" and m["data"].get("activityStatus") == 2, 3.0)

        print("[-] config change bumps configVersion")
        recv_until(ws, lambda m: False, 0.5)  # drain
        activities = os.path.join(hub.data_dir, "config", "activities.json")
        os.utime(activities, (time.time() + 5, time.time() + 5))  # the watcher compares mtime and size
        n, _ = recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify", 5.0)
        check(n is not None and n["data"].get("configVersion") == cfg_version, "config_changed: digest notify (same inventory, same configVersion)",
              str(n)[:120] if n else "no notify")
        print("[-] 6.1 toggle")
        st, _ = hub.http_post("/api/settings", {"legacy_enabled": False})
        check(st == 200, "POST /api/settings legacy_enabled false")
        time.sleep(0.3)
        closed, _ = recv_until(ws, lambda m: False, 1.0)
        try:
            ws.send_text("{}")
            ws.send_text("{}")
            alive = True
            ws.recv_json(timeout=0.5)
        except Exception:
            alive = False
        check(not port_open(LEGACY_PORT), "listener closed")
        check(not alive, "legacy client closed")
        st, _ = hub.http_post("/api/settings", {"legacy_enabled": True})
        time.sleep(0.3)
        check(port_open(LEGACY_PORT), "listener open again")
        ws3 = legacy_ws(REMOTE_ID)
        first, _ = recv_until(ws3, lambda m: m.get("type") == "connect.stateDigest?notify", 3.0)
        check(first is not None and first["data"].get("activityId") == "11", "new client after the toggle gets the digest", str(first)[:120])
        ws3.close()
        ws2.close()
        plain.close()

    print("[-] changeChannel with a channel device (listed in deviceIds only), derived remote id, large config")
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        devs = json.load(f)
    with open(os.path.join(SAMPLE_CONFIG, "activities.json")) as f:
        acts = json.load(f)
    for d in devs["devices"]:
        raw = d["commands"][0]["raw"]
        if d.get("id") == "2":
            # the digits live on a device that activity 10 lists in deviceIds, not in its sequences
            for n in range(10):
                d["commands"].append({"name": str(n), "raw": raw, "port": 7, "repeats": 1})
        if d.get("id") == "1":
            # enough commands for a stock configuration object over 64 KB (one WebSocket frame)
            for n in range(400):
                d["commands"].append({"name": "Extra%03d" % n, "raw": raw, "port": 7, "repeats": 1})
    for a in acts["activities"]:
        if a.get("id") == "10":
            a["deviceIds"] = ["2"]
    import tempfile
    log_path = os.path.join(tempfile.mkdtemp(prefix="hbus_"), "hub.log")
    with HubHarness(port=PORT, args=["--legacy-port", str(LEGACY_PORT), "--ssdp-port", "0"], log_path=log_path,
                    extra_config_files={"settings.json": settings(True), "devices.json": devs, "activities.json": acts}) as hub:
        st, _, prov = provision(hub)
        rid_derived = prov.get("data", {}).get("activeRemoteId", "")
        check(rid_derived.isdigit() and 0 < int(rid_derived) < 2 ** 31, "activeRemoteId derived from hub_id without the stock file", rid_derived)
        ws = legacy_ws(rid_derived)
        recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify", 3.0)
        rid = hbus(ws, "vnd.logitech.harmony/vnd.logitech.harmony.engine?config", remote_id=rid_derived)
        r, _ = reply_for(ws, rid, 10.0)
        cfg_len = len(json.dumps(r)) if r else 0
        dev1 = [d for d in (r or {}).get("data", {}).get("device", []) if d.get("label") == "Living Room TV"]
        check(r is not None and r.get("code") == 200 and cfg_len > 65535, "configuration object over 64 KB arrives in one frame (%d bytes)" % cfg_len)
        check(len(dev1) == 1 and sum(len(g.get("function", [])) for g in dev1[0].get("controlGroup", [])) >= 400,
              "its 400 extra commands are all there")
        rid = hbus(ws, "harmony.activityengine?runactivity", {"async": "true", "timestamp": 0, "activityId": "10"}, remote_id=rid_derived)
        recv_until(ws, lambda m: m.get("type") == "connect.stateDigest?notify" and m["data"].get("activityStatus") == 2, 15.0)
        time.sleep(0.5)
        with open(log_path) as f:
            sent_before = f.read().count("[DRY-RUN] IR ")
        rid = hbus(ws, "harmony.engine?changeChannel", {"timestamp": 0, "channel": "12"}, remote_id=rid_derived)
        r, _ = reply_for(ws, rid, 2.0)
        check(r is not None and r.get("code") == 200 and r.get("data") == {}, "changeChannel 12 with the channel device from deviceIds: 200", str(r))
        time.sleep(0.8)
        with open(log_path) as f:
            sent_after = f.read().count("[DRY-RUN] IR ")
        check(sent_after - sent_before == 2, "two digits sent 200 ms apart (daemon log)", "%d -> %d" % (sent_before, sent_after))
        rid = hbus(ws, "harmony.engine?changeChannel", {"timestamp": 0, "channel": "1a"}, remote_id=rid_derived)
        r, _ = reply_for(ws, rid, 2.0)
        check(r is not None and r.get("code") == 500, "changeChannel with a non-digit: 500")
        ws.close()

    print("\n%s" % ("ALL HBUS CHECKS PASSED" if failed == 0 else "%d HBUS CHECK(S) FAILED" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
