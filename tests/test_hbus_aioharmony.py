#!/usr/bin/env python3
"""
The Logitech protocol shim driven by the real aioharmony package (the one Home Assistant's core
harmony integration uses, pinned 1.0.8): connect (TCP probe, provisioning, WebSocket, config,
digest, discovery), hub id and name, activities, start_activity with its completion, callbacks,
send_commands, change_channel, sync, power_off, and a switch made elsewhere reaching the client.
Skipped when aioharmony is not installed (CI installs it; locally: pip install aioharmony==1.0.8).
"""

import asyncio
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402

try:
    import aioharmony.const
    import aioharmony.harmonyclient
    import aioharmony.hubconnector_websocket
    from aioharmony.harmonyapi import HarmonyAPI
    from aioharmony.const import ClientCallbackType, SendCommandDevice
except ImportError:
    print("[-] aioharmony not installed: test skipped")
    sys.exit(0)

PORT = 8996
LEGACY_PORT = 18091
REMOTE_ID = 7654321
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def settings():
    with open(os.path.join(SAMPLE_CONFIG, "settings.json")) as f:
        s = json.load(f)
    s["legacy_enabled"] = True
    return s


def devices_with_digits():
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        devs = json.load(f)
    for d in devs["devices"]:
        if d.get("id") == "1":
            raw = d["commands"][3]["raw"]
            for n in range(10):
                d["commands"].append({"name": str(n), "raw": raw, "port": 7, "repeats": 1})
    return devs


class Calls:
    def __init__(self):
        self.starting = []
        self.new = []
        self.config = []
        self.connected = []
        self.disconnected = []


def aid(call):
    """The activity id of a callback call: callbacks get one (id, name) tuple argument"""
    return call[0][0] if call and isinstance(call[0], tuple) else None


async def wait_for(lst, pred, timeout=15.0):
    for _ in range(int(timeout * 20)):
        if any(pred(x) for x in lst):
            return True
        await asyncio.sleep(0.05)
    return False


async def run(hub):
    calls = Calls()
    callbacks = ClientCallbackType(
        connect=lambda *a: calls.connected.append(a),
        disconnect=lambda *a: calls.disconnected.append(a),
        new_activity_starting=lambda *a: calls.starting.append(a),
        new_activity=lambda *a: calls.new.append(a),
        config_updated=lambda *a: calls.config.append(a),
    )
    client = HarmonyAPI(ip_address="127.0.0.1", callbacks=callbacks)
    ok = await client.connect()
    check(ok is True, "connect() (TCP probe, provisioning, WebSocket, config, digest, discovery)")
    if not ok:
        return
    try:
        await scenarios(hub, client, calls)
    finally:
        await client.close()
    check(len(calls.connected) >= 1, "connect callback fired")


async def scenarios(hub, client, calls):
    check(client.protocol == "WEBSOCKETS", "protocol WEBSOCKETS", client.protocol)
    check(client.hub_id == str(REMOTE_ID), "hub_id = activeRemoteId from the stock provisioning file", str(client.hub_id))
    check(client.name == "Test Hub", "name from discoveryinfo friendlyName", str(client.name))
    _, status = hub.http_get("/api/status")
    check(client.fw_version == status.get("version"), "fw_version = hubSwVersion", str(client.fw_version))
    check(client.account_id == "0" and client.email == "", "account_id and email", "%s %s" % (client.account_id, client.email))
    acts = {a["id"]: a["name"] for a in client.hub_config.activities}
    check(acts == {10: "Watch TV", 11: "Media Player", -1: "PowerOff"}, "activities parsed with int ids", str(acts))
    devs = {d["id"]: d["name"] for d in client.hub_config.devices}
    check(devs == {1: "Living Room TV", 2: "Media Player"}, "devices parsed with int ids", str(devs))
    check(client.get_device_id("Living Room TV") == 1 and client.get_activity_id("Watch TV") == 10, "name lookups")
    jc = client.json_config
    tv_cmds = jc.get("Devices", {}).get("Living Room TV", {}).get("commands", [])
    check({"PowerOn", "PowerOff", "InputHdmi1", "VolumeUp", "VolumeDown", "0", "9"} <= set(tv_cmds),
          "json_config decodes the function actions into command names", str(tv_cmds))
    check(jc.get("Activities", {}).get("10") == "Watch TV" and jc.get("Activities", {}).get("-1") == "PowerOff", "json_config activities")
    check(client.hub_config.config_version is not None, "config_version from the digest", str(client.hub_config.config_version))
    check(client.current_activity == (-1, "PowerOff"), "current activity PowerOff", str(client.current_activity))
    check(client.hub_config.hub_state.get("activityStatus") == 0, "hub_state from statedigest", str(client.hub_config.hub_state)[:80])

    print("[-] start_activity(10)")
    started, msg = await asyncio.wait_for(client.start_activity(10), 30)
    check(started is True, "start_activity returns True (startActivity 200 received)", str(msg))
    check(client.current_activity == (10, "Watch TV"), "current activity after the start", str(client.current_activity))
    check(await wait_for(calls.starting, lambda a: aid(a) == 10), "new_activity_starting callback (10)", str(calls.starting))
    check(await wait_for(calls.new, lambda a: aid(a) == 10), "new_activity callback (10) from startActivityFinished", str(calls.new))

    print("[-] send_commands, change_channel, sync")
    errors = await asyncio.wait_for(client.send_commands([SendCommandDevice(device=1, command="VolumeUp", delay=0)]), 10)
    check(errors == [], "send_commands VolumeUp: no error response", str(errors))
    errors = await asyncio.wait_for(client.send_commands([SendCommandDevice(device=1, command="NoSuchCommand", delay=0)]), 10)
    check(len(errors) == 1 and errors[0].code != "200", "an unknown command gets the code 500 error", str(errors))
    ok = await asyncio.wait_for(client.change_channel(12), 10)
    check(ok is True, "change_channel(12)")
    ok = await asyncio.wait_for(client.sync(), 60)
    check(ok is True, "sync() (setup.sync?None, then a full refresh)")
    check(client.current_activity == (10, "Watch TV"), "still on Watch TV after the refresh")

    print("[-] a switch made elsewhere (REST) reaches the client")
    calls.new.clear()
    hub.http_post("/api/activity", {"activity": "11"})
    check(await wait_for(calls.new, lambda a: aid(a) == 11), "new_activity callback (11) without a client request", str(calls.new))
    check(client.current_activity == (11, "Media Player"), "current activity follows", str(client.current_activity))

    print("[-] power_off")
    ok = await asyncio.wait_for(client.power_off(), 30)
    check(ok is True, "power_off returns True")
    check(client.current_activity == (-1, "PowerOff"), "current activity PowerOff after power_off", str(client.current_activity))
    check(await wait_for(calls.new, lambda a: aid(a) == -1), "new_activity callback (-1)")



def main():
    for mod, name in ((aioharmony.const, "DEFAULT_WS_HUB_PORT"), (aioharmony.hubconnector_websocket, "DEFAULT_HUB_PORT"),
                      (aioharmony.harmonyclient, "DEFAULT_WS_HUB_PORT")):
        setattr(mod, name, str(LEGACY_PORT))
    print("[-] aioharmony %s against the shim on port %d" % (getattr(aioharmony, "__version__", "?"), LEGACY_PORT))
    with HubHarness(port=PORT, args=["--legacy-port", str(LEGACY_PORT), "--ssdp-port", "0"],
                    extra_config_files={"settings.json": settings(), "devices.json": devices_with_digits()},
                    extra_root_files={"luaworks/provision/preferences": {"remoteId": REMOTE_ID}}) as hub:
        asyncio.run(run(hub))
    print("\n%s" % ("ALL AIOHARMONY CHECKS PASSED" if failed == 0 else "%d AIOHARMONY CHECK(S) FAILED" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
