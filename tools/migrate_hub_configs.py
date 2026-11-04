#!/usr/bin/env python3
"""Harmony Hub v2.0 Configuration Migration Utility.

Migrates:
1. Stock Logitech configurations (/data/resources/DeviceList.json & ActivityList.json)
2. Codex v1.x configurations (/data/codex/)
3. Legacy backup bundles (harmony_backup_bundle.json)

Produces 100% canonical v2.0 schema files:
- /data/harmony/config/devices.json
- /data/harmony/config/activities.json
- /data/harmony/config/settings.json
- /data/harmony/mqtt.json
- /data/harmony/remote_slot1_map.json
- /data/harmony/remote_slot2_map.json
- /data/harmony/bt_remote_map.json

A legacy bt-devices.json (Bluetooth device -> host address) is folded into devices.json,
the only device-to-host mapping; it is not written any more.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import pathlib
import re
import shutil
import sys
import tarfile
from typing import Any, Dict, List, Optional, Tuple

# Logitech "Transport" is a bitmask: 1 = IR, 32 = Bluetooth (33 = both, e.g. a TV with a BT keyboard)
TRANSPORT_IR = 1
TRANSPORT_BT = 32

# Delay for migrated input steps when the stock InputFeature has none (ms)
DEFAULT_INPUT_STEP_DELAY_MS = 300

# Button names hub_rf's map_button_code() produces (rf_keymap.c); map keys must use these names
HUB_BUTTON_NAMES = (
    "Back", "Blue", "ChannelDown", "ChannelUp", "DirectionDown", "DirectionLeft", "DirectionRight",
    "DirectionUp", "DownArrow", "Dvr", "Exit", "FastForward", "Green", "Guide", "Ha1", "Ha2", "Ha3", "Ha4",
    "Info", "Live", "Menu", "MovieActivity", "MusicActivity", "Number0", "Number1", "Number2", "Number3",
    "Number4", "Number5", "Number6", "Number7", "Number8", "Number9", "NumberEnter", "NumberPlus", "Pause",
    "Play", "PowerOffActivity", "Record", "Red", "Rewind", "RockerDown", "RockerUp", "Select", "Stop",
    "UpArrow", "VolumeDown", "VolumeMute", "VolumeUp", "WatchTVActivity", "Yellow",
)
_HUB_BUTTON_BY_LOWER = {n.lower(): n for n in HUB_BUTTON_NAMES}

# Stock MapList ButtonKey names that differ from hub_rf's names (compared case-insensitively)
STOCK_BUTTON_KEY_ALIASES = {"prevchannel": "Back"}

# The config push sends at most 48 buttons per remote (rf_config_push.c)
SCREEN_BUTTONS_PER_REMOTE = 48

# Fallback map when a stock activity has no ButtonMap: hub button -> stock command names to try
DEFAULT_BUTTON_COMMANDS: Dict[str, List[str]] = {
    "VolumeUp": ["VolumeUp"], "VolumeDown": ["VolumeDown"], "VolumeMute": ["Mute", "VolumeMute"],
    "ChannelUp": ["ChannelUp", "PageUp"], "ChannelDown": ["ChannelDown", "PageDown"],
    "DirectionUp": ["DirectionUp"], "DirectionDown": ["DirectionDown"],
    "DirectionLeft": ["DirectionLeft"], "DirectionRight": ["DirectionRight"],
    "Select": ["Select", "OK"], "Back": ["Back", "Return", "PrevChannel"], "Exit": ["Exit"],
    "Menu": ["Menu"], "Guide": ["Guide"], "Info": ["Info"],
    "Play": ["Play"], "Pause": ["Pause"], "Stop": ["Stop"], "Rewind": ["Rewind"],
    "FastForward": ["FastForward"], "Record": ["Record"], "Dvr": ["DVR"], "Live": ["Live", "LiveTV"],
    "Red": ["Red"], "Green": ["Green"], "Yellow": ["Yellow"], "Blue": ["Blue"],
}
DEFAULT_BUTTON_COMMANDS.update({f"Number{i}": [str(i)] for i in range(10)})
VOLUME_BUTTONS = ("VolumeUp", "VolumeDown", "VolumeMute")
CHANNEL_BUTTONS = ("ChannelUp", "ChannelDown") + tuple(f"Number{i}" for i in range(10))

# Stock activity roles, in the order that picks the device controlling an activity
MAIN_ROLE_TYPES = (
    "PlayGameActivityRole", "PlayMovieActivityRole", "PlayMusicActivityRole", "SmartTVActivityRole",
    "ChannelChangingActivityRole", "DisplayActivityRole",
)


def convert_hid_code(code: str) -> str:
    """Convert legacy raw hex or Logitech HID code to canonical HID:07:xxxx."""
    if not code:
        return ""
    code = code.strip()
    if code.startswith("HID:"):
        return code
    # e.g. G:HID(0x07, 0x0028) or similar
    m = re.search(r"0x([0-9a-fA-F]{2})\s*,\s*0x([0-9a-fA-F]{4})", code)
    if m:
        page, usage = m.group(1).upper(), m.group(2).upper()
        return f"HID:{page}:{usage}"
    # e.g. G:HID 16 Bit:()(07000027)():1 or (070027)
    m = re.search(r"\(([0-9a-fA-F]{2})(?:[0-9a-fA-F]{2})?([0-9a-fA-F]{4})\)", code)
    if m:
        page, usage = m.group(1).upper(), m.group(2).upper()
        return f"HID:{page}:{usage}"
    return code


def resolve_activity_icon(name: str, type_str: str) -> str:
    """Determine canonical activity icon from activity name and type."""
    combined = f"{name} {type_str}".lower()
    if any(k in combined for k in ["shield", "nvidia"]):
        return "shield.png"
    if any(k in combined for k in ["ps5", "playstation", "ps4", "ps3", "sony playstation"]):
        return "playstation.png"
    if any(k in combined for k in ["switch", "nintendo", "wii"]):
        return "switch.png"
    if any(k in combined for k in ["xbox", "one", "series"]):
        return "xbox.png"
    if any(k in combined for k in ["apple", "appletv", "atv"]):
        return "appletv.png"
    if any(k in combined for k in ["kodi", "media", "stream"]):
        return "kodi.png"
    if any(k in combined for k in ["tv", "television", "watch tv"]):
        return "watch_tv.png"
    if any(k in combined for k in ["movie", "cinema", "film"]):
        return "watch_movie.png"
    if any(k in combined for k in ["music", "listen", "audio", "stereo"]):
        return "listen_music.png"
    if any(k in combined for k in ["power", "off"]):
        return "power_off.png"
    return "generic.png"


def _stock_feature(item: Dict[str, Any], ftype: str) -> Dict[str, Any]:
    """Return the stock DeviceFeatures entry of the given __type (empty dict if absent)."""
    for f in item.get("DeviceFeatures") or []:
        if isinstance(f, dict) and f.get("__type") == ftype:
            return f
    return {}


def _stock_action_steps(actions: Any) -> List[Dict[str, Any]]:
    """Convert stock IRPressAction lists (power, input) into harmony-ng Command steps."""
    steps: List[Dict[str, Any]] = []
    for a in sorted([x for x in actions or [] if isinstance(x, dict)], key=lambda x: int(x.get("Order") or 0)):
        cname = a.get("IRCommandName") or a.get("CommandName") or ""
        if not cname:
            continue
        dur = a.get("Duration")
        steps.append({"type": "Command", "command": cname, "delay": int(dur) if isinstance(dur, int) and dur > 0 else 0,
                      "order": len(steps)})
    return steps


def migrate_stock_devices(device_list_path: str, function_list_path: Optional[str] = None,
                          warnings: Optional[List[str]] = None) -> Dict[str, Any]:
    """Convert stock Logitech DeviceList.json to canonical devices.json."""
    if warnings is None:
        warnings = []
    with open(device_list_path, "r", encoding="utf-8") as f:
        stock = json.load(f)

    # Optional function list map for resolving IR labels if needed
    fn_map = {}
    if function_list_path and os.path.isfile(function_list_path):
        try:
            with open(function_list_path, "r", encoding="utf-8") as ff:
                fns = json.load(ff).get("Function", [])
                for fn in fns:
                    fid = str(fn.get("Id-") or fn.get("Id") or "")
                    if fid:
                        fn_map[fid] = fn.get("Name", "")
        except Exception:
            pass

    dwf = stock.get("DevicesWithFeatures", [])
    clean_devices = []

    for item in dwf:
        d = item.get("Device")
        if not d:
            continue
        raw_id = str(d.get("Id-") or d.get("Id") or "").strip()
        if not raw_id:
            continue

        name = d.get("Name") or d.get("FriendlyName") or "Device"
        mfg = d.get("Manufacturer") or ""
        mdl = d.get("Model") or ""
        disp_type = d.get("DeviceTypeDisplayName") or d.get("Type") or ""
        transport = int(d.get("Transport") or TRANSPORT_IR)

        # Transport is a bitmask. A device with IR stays IR (e.g. 33 = IR + Bluetooth keyboard);
        # only a Bluetooth-only device becomes BT.
        is_mqtt = "mqtt" in disp_type.lower()
        if transport & (TRANSPORT_IR | TRANSPORT_BT):
            is_bt = not (transport & TRANSPORT_IR)
        else:
            is_bt = "bluetooth" in disp_type.lower()
            if not is_mqtt:
                warnings.append(f"device '{name}': unsupported stock transport {transport}, migrated as {'BT' if is_bt else 'IR'}")
        clean_type = "BT" if is_bt else ("MQTT" if is_mqtt else "IR")
        cmd_transport = TRANSPORT_BT if is_bt else TRANSPORT_IR

        cp = d.get("ControlPort")
        control_port = int(cp) if cp is not None else 7

        inter_device = int(d.get("InterDeviceDelay") or 500)
        inter_key = int(d.get("InterKeyDelay") or 100)
        hold_inter_key = int(d.get("HoldInterKeyDelay") or 200)
        default_inter = int(d.get("DefaultInterDeviceDelay") or 500)
        power = _stock_feature(item, "PowerFeature")
        power_on_delay = int(power.get("PowerOnDelay") or d.get("PowerOnDelay") or 1500)

        bdaddr = d.get("BTAddress") or ""

        # Commands
        raw_cmds = item.get("Commands", [])
        clean_cmds = []
        has_connect, has_disconnect = False, False
        skipped_cmds: List[str] = []

        for idx, c in enumerate(raw_cmds):
            cname = c.get("Name") or ""
            if not cname:
                continue
            # A harmony-ng device has one transport: drop commands of the other one
            # (e.g. the Bluetooth HID commands of an IR + Bluetooth TV)
            ctt = c.get("TransportType")
            if isinstance(ctt, int) and ctt and not (ctt & cmd_transport) and not is_mqtt:
                skipped_cmds.append(cname)
                continue
            if cname.lower() == "connect":
                has_connect = True
            if cname.lower() == "disconnect":
                has_disconnect = True

            key_code = convert_hid_code(c.get("KeyCode") or "")
            raw_str = convert_hid_code(c.get("Raw") or "")

            cmd_obj: Dict[str, Any] = {
                "name": cname,
                "id": c.get("Id") or c.get("Id-") or idx,
                "keyCode": key_code,
                "raw": raw_str
            }
            if not is_bt and not is_mqtt:
                cmd_obj["port"] = control_port
                cmd_obj["repeats"] = int(c.get("Repeats") or 3)

            clean_cmds.append(cmd_obj)

        if is_bt:
            if not has_connect:
                clean_cmds.append({"name": "Connect", "id": 9001, "keyCode": "CONNECT", "raw": "CONNECT"})
            if not has_disconnect:
                clean_cmds.append({"name": "Disconnect", "id": 9002, "keyCode": "DISCONNECT", "raw": "DISCONNECT"})

        if skipped_cmds:
            warnings.append(f"device '{name}': {len(skipped_cmds)} {'IR' if is_bt else 'Bluetooth'} commands not migrated "
                            f"(device migrated as {clean_type}): {', '.join(skipped_cmds)}")

        # Power: stock keeps it in the PowerFeature. Discrete on/off actions become the device's
        # power sequences; a toggle-only device keeps empty sequences so harmony-ng falls back to
        # its PowerToggle/Power command.
        p_on = _stock_action_steps(power.get("PowerOnActions"))
        p_off = _stock_action_steps(power.get("PowerOffActions"))
        toggle = _stock_action_steps(power.get("PowerToggleActions"))
        if toggle and not p_on and not p_off and toggle[0]["command"].lower() not in ("powertoggle", "power"):
            p_on, p_off = toggle, [dict(st) for st in toggle]
        is_always_on = bool(power.get("IsPowerAlwaysOn"))

        dev_obj: Dict[str, Any] = {
            "id": raw_id,
            "name": name,
            "manufacturer": mfg,
            "model": mdl,
            "type": clean_type,
            "controlPort": control_port,
            "icon": "",
            "interDeviceDelay": inter_device,
            "interKeyDelay": inter_key,
            "holdInterKeyDelay": hold_inter_key,
            "defaultInterDeviceDelay": default_inter,
            "powerOnDelay": power_on_delay,
            "isPowerAlwaysOn": is_always_on,
            "bdaddr": bdaddr,
            "btHost": bdaddr,
            "btAutoReconnect": 1 if is_bt else -1,
            "btReconnectDurationSec": 0,
            "btReconnectDelaySec": -1,
            "btTriggerDeviceId": "",
            "btTriggerCommandName": "",
            "powerOnSequence": p_on,
            "powerOffSequence": p_off,
            "commands": clean_cmds
        }

        if is_mqtt:
            dev_obj["mqttEnabled"] = True
            dev_obj["mqttTopic"] = "{base_topic}/button/{device}/{command}"
            dev_obj["mqttPulseMs"] = 1000

        clean_devices.append(dev_obj)

    return {"devices": clean_devices}


def load_stock_device_inputs(device_list_path: Optional[str]) -> Dict[str, Dict[str, Any]]:
    """Read each stock device's InputFeature: {stock_device_id: {"delay": ms, "type": n, "inputs": {name: steps}}}."""
    out: Dict[str, Dict[str, Any]] = {}
    if not device_list_path or not os.path.isfile(device_list_path):
        return out
    with open(device_list_path, "r", encoding="utf-8") as f:
        stock = json.load(f)
    for item in stock.get("DevicesWithFeatures", []):
        d = item.get("Device") or {}
        did = str(d.get("Id-") or d.get("Id") or "").strip()
        feat = _stock_feature(item, "InputFeature")
        if not did or not feat:
            continue
        inputs = {}
        for inp in feat.get("Inputs") or []:
            iname = str(inp.get("InputName") or "").strip()
            if iname and iname.lower() not in inputs:
                inputs[iname.lower()] = _stock_action_steps(inp.get("Actions"))
        out[did] = {"delay": int(feat.get("InputDelay") or 0), "type": feat.get("InputType"), "inputs": inputs,
                    "name": d.get("Name") or did}
    return out


def _sorted_roles(a: Dict[str, Any]) -> List[Dict[str, Any]]:
    roles = [r for r in a.get("Roles") or [] if isinstance(r, dict)]
    return sorted(roles, key=lambda r: int(r.get("PowerOnOrder") or r.get("order") or 999))


def _role_device(r: Dict[str, Any]) -> str:
    return str(r.get("DeviceId-") or r.get("DeviceId") or r.get("deviceId") or "").strip()


def stock_input_steps(a: Dict[str, Any], device_inputs: Dict[str, Dict[str, Any]],
                      warnings: List[str]) -> List[Dict[str, Any]]:
    """Build the steps that select each role's input (Roles[].SelectedInput), in power-on order."""
    steps: List[Dict[str, Any]] = []
    done = set()
    for r in _sorted_roles(a):
        did = _role_device(r)
        sel = r.get("SelectedInput") or {}
        iname = str(sel.get("Name") or "").strip() if isinstance(sel, dict) else ""
        if not did or not iname or did in done:
            continue
        done.add(did)
        dev_inp = device_inputs.get(did)
        if not dev_inp:
            continue
        actions = dev_inp["inputs"].get(iname.lower())
        if not actions:
            warnings.append(f"activity '{a.get('Name')}': input '{iname}' on '{dev_inp['name']}' not migrated "
                            f"(no direct input commands, stock InputType {dev_inp['type']})")
            continue
        delay = dev_inp["delay"] if dev_inp["delay"] > 0 else DEFAULT_INPUT_STEP_DELAY_MS
        for st in actions:
            steps.append({"type": "Command", "deviceId": did, "command": st["command"],
                          "delay": st["delay"] or delay})
    return steps


def stock_activity_roles(activity_list_path: str, id_map: Dict[str, str]) -> Dict[str, Dict[str, str]]:
    """Return {new_activity_id: {role __type: stock_device_id}} from ActivityList.json."""
    with open(activity_list_path, "r", encoding="utf-8") as f:
        stock = json.load(f)
    arr = stock.get("Activities") or (stock if isinstance(stock, list) else [])
    out: Dict[str, Dict[str, str]] = {}
    for entry in arr:
        a = entry.get("Activity") if isinstance(entry, dict) and "Activity" in entry else entry
        if not isinstance(a, dict):
            continue
        new_id = id_map.get(str(a.get("Id-") or a.get("Id") or "").strip())
        if not new_id:
            continue
        roles: Dict[str, str] = {}
        for r in _sorted_roles(a):
            if r.get("__type") and _role_device(r):
                roles.setdefault(str(r["__type"]), _role_device(r))
        out[new_id] = roles
    return out


def migrate_stock_activities(activity_list_path: str, device_list_path: Optional[str] = None,
                             warnings: Optional[List[str]] = None) -> Tuple[Dict[str, Any], Dict[str, str]]:
    """Convert stock Logitech ActivityList.json to canonical activities.json with short IDs.

    With device_list_path, each activity's start sequence first selects the inputs from its
    roles (Roles[].SelectedInput, using the devices' InputFeature actions), then runs the
    stock EnterActions.

    Returns:
        (canonical_activities_dict, id_mapping_dict)
    """
    if warnings is None:
        warnings = []
    device_inputs = load_stock_device_inputs(device_list_path)
    with open(activity_list_path, "r", encoding="utf-8") as f:
        stock = json.load(f)

    arr = stock.get("Activities") or (stock if isinstance(stock, list) else [])
    clean_activities = []
    id_map: Dict[str, str] = {}
    next_short_id = 1

    for entry in arr:
        a = entry.get("Activity") if isinstance(entry, dict) and "Activity" in entry else entry
        if not isinstance(a, dict):
            continue

        old_id = str(a.get("Id-") or a.get("Id") or "").strip()
        if not old_id or old_id == "0":
            continue

        if old_id == "-1":
            new_id = "-1"
        else:
            new_id = str(next_short_id)
            next_short_id += 1
        id_map[old_id] = new_id

        name = a.get("Name") or ("PowerOff" if new_id == "-1" else "Activity")
        disp_type = a.get("SuggestedDisplay") or a.get("Type") or "Generic"
        order = int(a.get("ActivityOrder") or a.get("Order") or 0)
        icon = a.get("Icon") or resolve_activity_icon(name, disp_type)

        # deviceIds: sort roles by PowerOnOrder
        roles = a.get("Roles") or []
        sorted_roles = sorted(roles, key=lambda r: int(r.get("PowerOnOrder") or r.get("order") or 999))
        dev_ids = []
        for r in sorted_roles:
            did = str(r.get("DeviceId-") or r.get("DeviceId") or r.get("deviceId") or "").strip()
            if did and did not in dev_ids:
                dev_ids.append(did)

        # Start sequence: inputs from the roles, then the stock EnterActions
        start_seq = a.get("startSequence") or a.get("StartSequence") or a.get("EnterActions") or []
        norm_start = []
        for st in stock_input_steps(a, device_inputs, warnings):
            st.update({"id": 9000 + len(norm_start), "order": len(norm_start)})
            norm_start.append(st)
        for idx, s in enumerate(start_seq):
            cname = s.get("command") or s.get("CommandName") or s.get("Command") or ""
            stype = s.get("type") or s.get("Type") or ""
            is_sw = (stype.lower() in ["switched", "activityswitched"] or cname.lower() == "switched")
            is_dl = (stype.lower() == "delay" or s.get("__type") == "DelayActivityAction" or not is_sw and stype.lower() == "delay")
            raw_del = s.get("delay")
            if raw_del is None:
                raw_del = s.get("delay_ms") or s.get("delayMs") or s.get("Delay") or s.get("Duration") or 0
            del_val = int(raw_del) if str(raw_del).isdigit() else 0

            step_type = "Switched" if is_sw else ("Delay" if is_dl else "Command")
            step_obj = {
                "id": s.get("Id") or s.get("id") or idx,
                "type": step_type,
                "deviceId": str(s.get("DeviceId-") or s.get("DeviceId") or s.get("deviceId") or "0"),
                "command": "Switched" if is_sw else cname,
                "delay": del_val,
                "order": len(norm_start)
            }
            norm_start.append(step_obj)

        # Stop sequence
        stop_seq = a.get("stopSequence") or a.get("StopSequence") or a.get("LeaveActions") or []
        norm_stop = []
        for idx, s in enumerate(stop_seq):
            cname = s.get("command") or s.get("CommandName") or s.get("Command") or ""
            stype = s.get("type") or s.get("Type") or ""
            is_sw = (stype.lower() in ["switched", "activityswitched"] or cname.lower() == "switched")
            is_dl = (stype.lower() == "delay" or s.get("__type") == "DelayActivityAction" or not is_sw and stype.lower() == "delay")
            raw_del = s.get("delay")
            if raw_del is None:
                raw_del = s.get("delay_ms") or s.get("delayMs") or s.get("Delay") or s.get("Duration") or 0
            del_val = int(raw_del) if str(raw_del).isdigit() else 0

            step_type = "Switched" if is_sw else ("Delay" if is_dl else "Command")
            step_obj = {
                "id": s.get("Id") or s.get("id") or idx,
                "type": step_type,
                "deviceId": str(s.get("DeviceId-") or s.get("DeviceId") or s.get("deviceId") or "0"),
                "command": "Switched" if is_sw else cname,
                "delay": del_val,
                "order": int(s.get("Order") or s.get("ActionOrder") or idx)
            }
            norm_stop.append(step_obj)

        clean_activities.append({
            "id": new_id,
            "name": name,
            "type": disp_type,
            "icon": icon,
            "order": order,
            "deviceIds": dev_ids,
            "startSequence": norm_start,
            "stopSequence": norm_stop
        })

    return {"activities": clean_activities}, id_map


def _remap_device_refs(obj: Any, dev_map: Dict[str, str]) -> Any:
    """Recursively rewrite device id references (deviceId / DeviceId / DeviceId- keys)."""
    if isinstance(obj, dict):
        out = {}
        for k, v in obj.items():
            if k in ("deviceId", "DeviceId", "DeviceId-", "btTriggerDeviceId") and str(v) in dev_map:
                out[k] = dev_map[str(v)]
            else:
                out[k] = _remap_device_refs(v, dev_map)
        return out
    if isinstance(obj, list):
        return [_remap_device_refs(v, dev_map) for v in obj]
    return obj


def shorten_stock_device_ids(devices_obj: Dict[str, Any], activities_obj: Optional[Dict[str, Any]]) -> Dict[str, str]:
    """Replace stock Logitech device ids ("75563901") with short ids ("1", "2", ...), like activities.

    The RF protocol only carries the first 4 characters of a device id, so long ids never
    match on the Hub. Rewrites the devices and every reference in the activities in place
    and returns the old -> new id map.
    """
    dev_map: Dict[str, str] = {}
    next_id = 1
    for d in devices_obj.get("devices", []):
        old_id = str(d.get("id", ""))
        if not old_id:
            continue
        dev_map[old_id] = str(next_id)
        next_id += 1
    if not dev_map:
        return dev_map

    devices_obj["devices"] = [_remap_device_refs(d, dev_map) for d in devices_obj.get("devices", [])]
    for d in devices_obj["devices"]:
        old_id = str(d.get("id", ""))
        if old_id in dev_map:
            d["id"] = dev_map[old_id]

    if activities_obj:
        acts = []
        for a in activities_obj.get("activities", []):
            a = _remap_device_refs(a, dev_map)
            a["deviceIds"] = [dev_map.get(str(did), str(did)) for did in a.get("deviceIds", [])]
            acts.append(a)
        activities_obj["activities"] = acts
    return dev_map


def _button_entry(device_id: str, command: str) -> Dict[str, Any]:
    """One button in the flat map format hub_rf reads (activities -> id -> button name -> entry)."""
    return {"type": "command", "action": "device_cmd", "deviceId": str(device_id), "command": command}


def _find_command(dev: Optional[Dict[str, Any]], candidates: List[str]) -> Optional[str]:
    if not dev:
        return None
    names = {str(c.get("name", "")).lower(): c.get("name") for c in dev.get("commands", [])}
    for cand in candidates:
        if cand.lower() in names:
            return names[cand.lower()]
    return None


def generate_default_remote_map(activities_obj: Dict[str, Any], devices_obj: Dict[str, Any], remote_name: str,
                                roles: Optional[Dict[str, Dict[str, str]]] = None,
                                only_activities: Optional[List[str]] = None) -> Dict[str, Any]:
    """Generate a remote button map (flat hub_rf format) from the activities' devices.

    Used when there is no stock MapList.json ButtonMap for an activity. With roles
    ({activity_id: {role __type: device_id}}), volume keys go to the Volume role's device,
    channel keys to the ChannelChanging role's device and the rest to the activity's main
    device (PlayGame, PlayMovie, ... Display); otherwise the first device with a matching
    command wins.
    """
    acts = activities_obj.get("activities", [])
    devs = {str(d.get("id")): d for d in devices_obj.get("devices", [])}
    roles = roles or {}

    act_mappings: Dict[str, Any] = {}
    for act in acts:
        aid = str(act.get("id"))
        if only_activities is not None and aid not in only_activities:
            continue
        dev_ids = [str(did) for did in act.get("deviceIds", []) if str(did) in devs]
        act_roles = roles.get(aid, {})
        main_dev = next((act_roles[r] for r in MAIN_ROLE_TYPES if act_roles.get(r) in devs), None)

        buttons: Dict[str, Any] = {}
        for button, candidates in DEFAULT_BUTTON_COMMANDS.items():
            if button in VOLUME_BUTTONS:
                preferred = act_roles.get("VolumeActivityRole")
            elif button in CHANNEL_BUTTONS:
                preferred = act_roles.get("ChannelChangingActivityRole") or main_dev
            else:
                preferred = main_dev
            order = ([preferred] if preferred in devs else []) + [d for d in dev_ids if d != preferred]
            for did in order:
                cmd = _find_command(devs[did], candidates)
                if cmd:
                    buttons[button] = _button_entry(did, cmd)
                    break
        act_mappings[aid] = buttons

    return {"remote": remote_name, "activities": act_mappings, "devices": {}}


def generate_stock_remote_maps(map_list_path: str, devices_obj: Dict[str, Any], dev_map: Dict[str, str],
                               act_map: Dict[str, str], warnings: List[str]) -> List[Dict[str, Any]]:
    """Convert the stock MapList.json ButtonMaps into hub_rf remote maps, one per stock remote.

    Activity maps become activities -> id -> button, device maps (device browser) become
    devices -> id -> button, an activity's screen buttons become its screen_buttons list.
    hub_rf has no long press, double press or gestures, so those are reported and dropped.
    Maps are returned with the remote that has screen buttons first.
    """
    with open(map_list_path, "r", encoding="utf-8") as f:
        stock = json.load(f)
    devs = {str(d.get("id")): d for d in devices_obj.get("devices", [])}
    remotes: Dict[str, Dict[str, Any]] = {}

    for bm in stock.get("ButtonMaps") or []:
        if not isinstance(bm, dict):
            continue
        ident = str(bm.get("ButtonMapIdentifier") or "")
        m = re.search(r"(Activity|Device)(-?\d+)$", ident)
        if not m:
            continue
        is_act = m.group(1) == "Activity"
        ctx = (act_map if is_act else dev_map).get(m.group(2))
        if not ctx:
            warnings.append(f"button map {ident}: unknown {m.group(1).lower()} {m.group(2)}, skipped")
            continue
        rid = str(bm.get("RemoteId-") or bm.get("RemoteId") or "0")
        rmap = remotes.setdefault(rid, {"activities": {}, "devices": {}, "_screen": {}})
        target = (rmap["activities"] if is_act else rmap["devices"]).setdefault(ctx, {})
        label = f"{m.group(1).lower()} {ctx}"
        dropped: Dict[str, int] = {}
        soft = []

        for b in bm.get("Buttons") or []:
            if not isinstance(b, dict):
                continue
            act = b.get("ButtonAction") or {}
            if act.get("__type") != "ButtonCommandAction":
                dropped["non-command buttons"] = dropped.get("non-command buttons", 0) + 1
                continue
            did = dev_map.get(str(act.get("DeviceId-")), str(act.get("DeviceId-")))
            cmd = _find_command(devs.get(did), [str(act.get("CommandName") or "")])
            if not cmd:
                warnings.append(f"{label}: command '{act.get('CommandName')}' not found on device {did}, skipped")
                continue
            entry = _button_entry(did, cmd)
            btype = b.get("__type")
            if btype == "HardRemoteButton":
                key = str(b.get("ButtonKey") or "")
                hub_key = STOCK_BUTTON_KEY_ALIASES.get(key.lower()) or _HUB_BUTTON_BY_LOWER.get(key.lower())
                if not hub_key:
                    warnings.append(f"{label}: stock key '{key}' has no hub_rf button name, skipped")
                    continue
                target[hub_key] = entry
                if b.get("ButtonLongPressAction") or b.get("ButtonDoublePressAction"):
                    dropped["long/double presses"] = dropped.get("long/double presses", 0) + 1
            elif btype == "SoftRemoteButton":
                if is_act:
                    menu = b.get("MenuItem") or {}
                    soft.append(((int(b.get("FunctionGroupType") or 0), int(menu.get("IndexInMenu") or 0)),
                                 dict(entry, label=str(b.get("TextOnRemote") or cmd)[:12])))
            else:
                dropped["gestures"] = dropped.get("gestures", 0) + 1

        if soft:
            soft.sort(key=lambda x: x[0])
            rmap["_screen"][ctx] = [e for _, e in soft]
        if dropped:
            warnings.append(f"{label}: not supported by hub_rf, dropped: "
                            + ", ".join(f"{n} {k}" for k, n in sorted(dropped.items())))

    maps = []
    for rid, rmap in remotes.items():
        screen = rmap.pop("_screen")
        if screen:
            per_act = max(1, SCREEN_BUTTONS_PER_REMOTE // len(screen))
            for ctx, items in screen.items():
                if len(items) > per_act:
                    warnings.append(f"activity {ctx}: {len(items)} screen buttons, kept the first {per_act} "
                                    f"(config push limit {SCREEN_BUTTONS_PER_REMOTE} per remote)")
                rmap["activities"].setdefault(ctx, {})["screen_buttons"] = items[:per_act]
        rmap["_screen_count"] = sum(len(v) for v in screen.values())
        rmap["remote"] = f"Stock remote {rid}"
        maps.append(rmap)
    maps.sort(key=lambda r: -r["_screen_count"])
    for r in maps:
        r.pop("_screen_count")
    return maps


def remap_remote_map_activities(map_obj: Dict[str, Any], id_map: Dict[str, str]) -> Dict[str, Any]:
    """Remap activity keys and references in a remote button map (RF or Bluetooth)."""
    if not map_obj or not isinstance(map_obj, dict):
        return map_obj

    acts = map_obj.get("activities", {})
    new_acts = {}
    for old_aid, bdata in acts.items():
        new_aid = id_map.get(str(old_aid), str(old_aid))
        if isinstance(bdata, dict):
            sub_dict = bdata.get("buttons") if "buttons" in bdata and isinstance(bdata.get("buttons"), dict) else bdata
            for btn_key, action in list(sub_dict.items()):
                if isinstance(action, dict):
                    for act_item in [action, action.get("short"), action.get("long")]:
                        if isinstance(act_item, dict):
                            for tag in ["targetActivity", "activityId"]:
                                if tag in act_item and str(act_item[tag]) in id_map:
                                    act_item[tag] = id_map[str(act_item[tag])]
        new_acts[new_aid] = bdata

    map_obj["activities"] = new_acts
    return map_obj


def check_step_commands(devices_obj: Dict[str, Any], activities_obj: Optional[Dict[str, Any]],
                        warnings: List[str]) -> None:
    """Warn about power and activity steps whose command doesn't exist on the target device."""
    devs = {str(d.get("id")): d for d in devices_obj.get("devices", [])}

    def check(where: str, did: str, steps: List[Dict[str, Any]]) -> None:
        for st in steps or []:
            if str(st.get("type", "Command")).lower() not in ("command", "ircommand"):
                continue
            target = str(st.get("deviceId") or did)
            if target in devs and not _find_command(devs[target], [str(st.get("command") or "")]):
                warnings.append(f"{where}: step '{st.get('command')}' not found on device '{devs[target].get('name')}'")

    for did, d in devs.items():
        check(f"device '{d.get('name')}' power on", did, d.get("powerOnSequence"))
        check(f"device '{d.get('name')}' power off", did, d.get("powerOffSequence"))
    for a in (activities_obj or {}).get("activities", []):
        check(f"activity '{a.get('name')}' start", "", a.get("startSequence"))
        check(f"activity '{a.get('name')}' stop", "", a.get("stopSequence"))


def _complete_remote_map(map_obj: Dict[str, Any], activities_obj: Dict[str, Any], devices_obj: Dict[str, Any],
                         remote_name: str, roles: Dict[str, Dict[str, str]]) -> Dict[str, Any]:
    """Add default maps for activities the stock MapList has no ButtonMap for."""
    missing = [str(a.get("id")) for a in activities_obj.get("activities", [])
               if str(a.get("id")) not in map_obj.get("activities", {})]
    if missing:
        defaults = generate_default_remote_map(activities_obj, devices_obj, remote_name, roles, missing)
        map_obj.setdefault("activities", {}).update(defaults["activities"])
    map_obj["remote"] = remote_name
    return map_obj


def sanitize_devices_json(devices_obj: Dict[str, Any]) -> Dict[str, Any]:
    """Normalize devices to canonical v2.0 schema."""
    devs = devices_obj.get("devices") or (devices_obj if isinstance(devices_obj, list) else [])
    clean_devs = []

    for d in devs:
        if not isinstance(d, dict):
            continue
        did = str(d.get("id") or d.get("Id-") or d.get("Id") or "").strip()
        if not did:
            continue
        clean_dev = {
            "id": did,
            "name": d.get("name") or d.get("Name") or "Device",
            "type": d.get("type") or "IR",
            "manufacturer": d.get("manufacturer") or d.get("Manufacturer") or "",
            "model": d.get("model") or d.get("Model") or "",
            "controlPort": int(d.get("controlPort") or 7),
            "icon": d.get("icon") or "",
            "interDeviceDelay": int(d.get("interDeviceDelay") or 500),
            "interKeyDelay": int(d.get("interKeyDelay") or 100),
            "holdInterKeyDelay": int(d.get("holdInterKeyDelay") or 200),
            "defaultInterDeviceDelay": int(d.get("defaultInterDeviceDelay") or 500),
            "powerOnDelay": int(d.get("powerOnDelay") or 1500),
            "isPowerAlwaysOn": bool(d.get("isPowerAlwaysOn")),
            "bdaddr": d.get("bdaddr") or d.get("btHost") or "",
            "btHost": d.get("bdaddr") or d.get("btHost") or "",
            "btAutoReconnect": int(d.get("btAutoReconnect") if d.get("btAutoReconnect") is not None else 1),
            "btReconnectDurationSec": int(d.get("btReconnectDurationSec") or 0),
            "btReconnectDelaySec": int(d.get("btReconnectDelaySec") if d.get("btReconnectDelaySec") is not None else -1),
            "btTriggerDeviceId": d.get("btTriggerDeviceId") or "",
            "btTriggerCommandName": d.get("btTriggerCommandName") or "",
            "powerOnSequence": d.get("powerOnSequence") or [],
            "powerOffSequence": d.get("powerOffSequence") or [],
            "commands": []
        }
        for idx, c in enumerate(d.get("commands", [])):
            cname = c.get("name") or (c if isinstance(c, str) else "")
            if not cname:
                continue
            kcode = convert_hid_code(c.get("keyCode") if isinstance(c, dict) else "")
            raw = convert_hid_code(c.get("raw") if isinstance(c, dict) else "")
            cobj: Dict[str, Any] = {
                "name": cname,
                "id": c.get("id") if isinstance(c, dict) and c.get("id") is not None else idx,
                "keyCode": kcode,
                "raw": raw
            }
            if clean_dev["type"] == "IR":
                cobj["repeats"] = int(c.get("repeats") or 3) if isinstance(c, dict) else 3
                cobj["port"] = int(c.get("port") or clean_dev["controlPort"]) if isinstance(c, dict) else clean_dev["controlPort"]
            clean_dev["commands"].append(cobj)

        if d.get("mqttEnabled") or clean_dev["type"] == "MQTT":
            clean_dev["mqttEnabled"] = True
            clean_dev["mqttTopic"] = d.get("mqttTopic") or "{base_topic}/button/{device}/{command}"
            clean_dev["mqttPulseMs"] = int(d.get("mqttPulseMs") or 1000)

        clean_devs.append(clean_dev)

    return {"devices": clean_devs}


BDADDR_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")


def merge_bt_device_hosts(devices_obj: Optional[Dict[str, Any]], bt_obj: Any) -> int:
    """Copy host addresses from a legacy bt-devices.json into devices.json.

    The Hub no longer reads bt-devices.json: devices.json "bdaddr" is the only
    device-to-host mapping. As in hub_daemon's start-up migration (bt_migrate.c), only devices
    without a host get one. Returns the number of devices changed.
    """
    if not devices_obj:
        return 0
    entries = bt_obj if isinstance(bt_obj, list) else (bt_obj.get("devices") if isinstance(bt_obj, dict) else None)
    if not isinstance(entries, list):
        return 0
    by_id = {str(d.get("id")): d for d in devices_obj.get("devices", []) if isinstance(d, dict) and d.get("id") is not None}
    changed = 0
    for e in entries:
        if not isinstance(e, dict) or e.get("id") is None:
            continue
        addr = e.get("bdaddr") or e.get("btHost") or ""
        dev = by_id.get(str(e.get("id")))
        if dev is None or not isinstance(addr, str) or not BDADDR_RE.match(addr) or addr == "00:00:00:00:00:00":
            continue
        if dev.get("bdaddr") or dev.get("btHost"):
            continue
        # sanitize_devices_json() writes both keys with the same value
        dev["bdaddr"] = addr
        dev["btHost"] = addr
        changed += 1
    return changed


def sanitize_activities_json(activities_obj: Dict[str, Any]) -> Tuple[Dict[str, Any], Dict[str, str]]:
    """Ensure activity IDs are short (1, 2, 3...) and sequence steps use canonical delay."""
    acts = activities_obj.get("activities") or (activities_obj if isinstance(activities_obj, list) else [])
    clean_acts = []
    id_map: Dict[str, str] = {}
    next_id = 1

    # Check if shortening is required
    needs_short = False
    for a in acts:
        aid = str(a.get("id", ""))
        if aid and aid != "-1" and (len(aid) > 2 or int(aid) > 63):
            needs_short = True
            break

    for a in acts:
        if not isinstance(a, dict):
            continue
        old_id = str(a.get("id") or a.get("Id-") or a.get("Id") or "").strip()
        if not old_id or old_id == "0":
            continue

        if old_id == "-1":
            new_id = "-1"
        elif needs_short:
            new_id = str(next_id)
            next_id += 1
        else:
            new_id = old_id

        id_map[old_id] = new_id

        # Normalize startSequence
        start_seq = a.get("startSequence") or a.get("startSteps") or a.get("EnterActions") or []
        norm_start = []
        for idx, s in enumerate(start_seq):
            cname = s.get("command") or s.get("CommandName") or s.get("Command") or ""
            stype = s.get("type") or s.get("Type") or "Command"
            is_sw = (stype.lower() in ["switched", "activityswitched"] or cname.lower() == "switched")
            is_dl = (stype.lower() == "delay" or s.get("__type") == "DelayActivityAction" or not is_sw and stype.lower() == "delay")
            del_val = s.get("delay")
            if del_val is None:
                del_val = s.get("delay_ms") or s.get("delayMs") or s.get("Delay") or s.get("Duration") or 0
            del_val = int(del_val) if str(del_val).isdigit() else 0
            norm_start.append({
                "id": s.get("id") if s.get("id") is not None else idx,
                "type": "Switched" if is_sw else ("Delay" if is_dl else "Command"),
                "deviceId": str(s.get("deviceId") or s.get("DeviceId-") or s.get("DeviceId") or "0"),
                "command": "Switched" if is_sw else cname,
                "delay": del_val,
                "order": int(s.get("order") if s.get("order") is not None else idx)
            })

        # Normalize stopSequence
        stop_seq = a.get("stopSequence") or a.get("stopSteps") or a.get("LeaveActions") or []
        norm_stop = []
        for idx, s in enumerate(stop_seq):
            cname = s.get("command") or s.get("CommandName") or s.get("Command") or ""
            stype = s.get("type") or s.get("Type") or "Command"
            is_sw = (stype.lower() in ["switched", "activityswitched"] or cname.lower() == "switched")
            is_dl = (stype.lower() == "delay" or s.get("__type") == "DelayActivityAction" or not is_sw and stype.lower() == "delay")
            del_val = s.get("delay")
            if del_val is None:
                del_val = s.get("delay_ms") or s.get("delayMs") or s.get("Delay") or s.get("Duration") or 0
            del_val = int(del_val) if str(del_val).isdigit() else 0
            norm_stop.append({
                "id": s.get("id") if s.get("id") is not None else idx,
                "type": "Switched" if is_sw else ("Delay" if is_dl else "Command"),
                "deviceId": str(s.get("deviceId") or s.get("DeviceId-") or s.get("DeviceId") or "0"),
                "command": "Switched" if is_sw else cname,
                "delay": del_val,
                "order": int(s.get("order") if s.get("order") is not None else idx)
            })

        clean_acts.append({
            "id": new_id,
            "name": a.get("name") or ("PowerOff" if new_id == "-1" else "Activity"),
            "type": a.get("type") or "Generic",
            "icon": a.get("icon") or resolve_activity_icon(a.get("name") or "", a.get("type") or ""),
            "order": int(a.get("order") or 0),
            "deviceIds": [str(did) for did in a.get("deviceIds", [])],
            "startSequence": norm_start,
            "stopSequence": norm_stop
        })

    return {"activities": clean_acts}, id_map


def migrate_directory(source_root: str, dest_harmony: str) -> Dict[str, Any]:
    """Inspect any directory (or backup) and migrate all configs to canonical v2.0."""
    os.makedirs(os.path.join(dest_harmony, "config"), exist_ok=True)
    stats: Dict[str, Any] = {"devices": 0, "activities": 0, "slot1_map": False, "slot2_map": False, "bt_remote_map": False, "mqtt": False, "settings": False, "warnings": []}
    warnings: List[str] = stats["warnings"]

    # 1. Check for stock Logitech resources
    res_dir = None
    for cand in [
        os.path.join(source_root, "data", "resources"),
        os.path.join(source_root, "resources"),
        source_root
    ]:
        if os.path.isfile(os.path.join(cand, "DeviceList.json")):
            res_dir = cand
            break

    # 2. Check for codex / harmony custom configs
    cfg_dir = None
    for cand in [
        os.path.join(source_root, "data", "harmony"),
        os.path.join(source_root, "data", "codex"),
        os.path.join(source_root, "codex"),
        source_root
    ]:
        if os.path.isfile(os.path.join(cand, "config", "devices.json")) or os.path.isfile(os.path.join(cand, "devices.json")):
            cfg_dir = cand
            break

    devices_obj = None
    activities_obj = None
    id_map: Dict[str, str] = {}

    # Prioritize existing codex/harmony configs if present, else stock import
    if cfg_dir:
        dev_path = os.path.join(cfg_dir, "config", "devices.json")
        if not os.path.isfile(dev_path):
            dev_path = os.path.join(cfg_dir, "devices.json")
        if os.path.isfile(dev_path):
            with open(dev_path, "r", encoding="utf-8") as f:
                devices_obj = sanitize_devices_json(json.load(f))

        act_path = os.path.join(cfg_dir, "config", "activities.json")
        if not os.path.isfile(act_path):
            act_path = os.path.join(cfg_dir, "activities.json")
        if os.path.isfile(act_path):
            with open(act_path, "r", encoding="utf-8") as f:
                activities_obj, id_map = sanitize_activities_json(json.load(f))

    # If devices or activities not found in codex, try stock resources
    devices_from_stock = False
    if (not devices_obj or not devices_obj.get("devices")) and res_dir:
        dev_list = os.path.join(res_dir, "DeviceList.json")
        fn_list = os.path.join(res_dir, "FunctionList.json")
        devices_obj = migrate_stock_devices(dev_list, fn_list, warnings)
        devices_from_stock = True

    activities_from_stock = False
    if (not activities_obj or not activities_obj.get("activities")) and res_dir:
        act_list = os.path.join(res_dir, "ActivityList.json")
        dev_list = os.path.join(res_dir, "DeviceList.json") if devices_from_stock else None
        activities_obj, id_map = migrate_stock_activities(act_list, dev_list, warnings)
        activities_from_stock = True

    # Stock device ids are 8+ digits; give them short ids like the activities
    dev_map: Dict[str, str] = {}
    if devices_from_stock and devices_obj:
        dev_map = shorten_stock_device_ids(devices_obj, activities_obj)
        check_step_commands(devices_obj, activities_obj, warnings)

    # Stock remote maps (MapList.json) and activity roles, for a full stock import
    stock_maps: List[Dict[str, Any]] = []
    roles: Dict[str, Dict[str, str]] = {}
    if devices_from_stock and activities_from_stock and devices_obj and activities_obj and res_dir:
        roles = {aid: {rt: dev_map.get(did, did) for rt, did in r.items()}
                 for aid, r in stock_activity_roles(os.path.join(res_dir, "ActivityList.json"), id_map).items()}
        map_list = os.path.join(res_dir, "MapList.json")
        if os.path.isfile(map_list):
            stock_maps = generate_stock_remote_maps(map_list, devices_obj, dev_map, id_map, warnings)

    # Legacy bt-devices.json: its host addresses go into devices.json
    if cfg_dir and devices_obj:
        for cand_b in [os.path.join(cfg_dir, "bt-devices.json"), os.path.join(cfg_dir, "bt_devices.json")]:
            if os.path.isfile(cand_b):
                try:
                    with open(cand_b, "r", encoding="utf-8") as f:
                        merge_bt_device_hosts(devices_obj, json.load(f))
                except (OSError, ValueError) as e:
                    warnings.append(f"{os.path.basename(cand_b)} not merged: {e}")
                break

    # Write devices.json and activities.json
    if devices_obj:
        out_path = os.path.join(dest_harmony, "config", "devices.json")
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(devices_obj, f, indent=2)
        stats["devices"] = len(devices_obj.get("devices", []))

    if activities_obj:
        out_path = os.path.join(dest_harmony, "config", "activities.json")
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(activities_obj, f, indent=2)
        stats["activities"] = len(activities_obj.get("activities", []))

    # 3. Settings
    settings_written = False
    if cfg_dir:
        for cand_s in [os.path.join(cfg_dir, "config", "settings.json"), os.path.join(cfg_dir, "settings.json")]:
            if os.path.isfile(cand_s):
                shutil.copyfile(cand_s, os.path.join(dest_harmony, "config", "settings.json"))
                settings_written = True
                break
    if not settings_written:
        def_settings = {
            "hub_name": "Harmony Hub",
            "dry_run": 0,
            "bt_auto_reconnect": 1,
            "bt_reconnect_duration_sec": 0,
            "bt_reconnect_delay_sec": -1
        }
        with open(os.path.join(dest_harmony, "config", "settings.json"), "w", encoding="utf-8") as f:
            json.dump(def_settings, f, indent=2)
    stats["settings"] = True

    # 4. Remote maps (Slot 1 and Slot 2)
    # Slot 1: elite_remote_map.json or remote_slot1_map.json
    slot1_obj = None
    if cfg_dir:
        for cand1 in [
            os.path.join(cfg_dir, "remote_slot1_map.json"),
            os.path.join(cfg_dir, "elite_remote_map.json")
        ]:
            if os.path.isfile(cand1):
                with open(cand1, "r", encoding="utf-8") as f:
                    slot1_obj = json.load(f)
                break

    if slot1_obj:
        if id_map:
            slot1_obj = remap_remote_map_activities(slot1_obj, id_map)
    elif stock_maps:
        slot1_obj = _complete_remote_map(stock_maps[0], activities_obj, devices_obj, "Harmony Elite", roles)
    elif activities_obj and devices_obj:
        slot1_obj = generate_default_remote_map(activities_obj, devices_obj, "Harmony Elite", roles)

    if slot1_obj:
        with open(os.path.join(dest_harmony, "remote_slot1_map.json"), "w", encoding="utf-8") as f:
            json.dump(slot1_obj, f, indent=2)
        stats["slot1_map"] = True

    # Slot 2 (RF Harmony Companion / 2nd RF Remote)
    slot2_obj = None
    if cfg_dir:
        cand2 = os.path.join(cfg_dir, "remote_slot2_map.json")
        if os.path.isfile(cand2):
            with open(cand2, "r", encoding="utf-8") as f:
                slot2_obj = json.load(f)

    if slot2_obj:
        if id_map:
            slot2_obj = remap_remote_map_activities(slot2_obj, id_map)
    elif len(stock_maps) > 1:
        slot2_obj = _complete_remote_map(stock_maps[1], activities_obj, devices_obj, "Harmony Companion", roles)
    elif activities_obj and devices_obj and res_dir:
        # Auto-generate RF companion map only on initial stock conversion
        slot2_obj = generate_default_remote_map(activities_obj, devices_obj, "Harmony Companion", roles)

    if slot2_obj:
        with open(os.path.join(dest_harmony, "remote_slot2_map.json"), "w", encoding="utf-8") as f:
            json.dump(slot2_obj, f, indent=2)
        stats["slot2_map"] = True

    # 5. Bluetooth Remote Map (Physical Bluetooth HID remote, e.g. Homatics B25)
    if cfg_dir:
        bt_map_path = os.path.join(cfg_dir, "bt_remote_map.json")
        if os.path.isfile(bt_map_path):
            with open(bt_map_path, "r", encoding="utf-8") as f:
                bt_map_obj = json.load(f)
            if id_map:
                bt_map_obj = remap_remote_map_activities(bt_map_obj, id_map)
            with open(os.path.join(dest_harmony, "bt_remote_map.json"), "w", encoding="utf-8") as f:
                json.dump(bt_map_obj, f, indent=2)
            stats["bt_remote_map"] = True

    # 6. MQTT config
    if cfg_dir:
        for cand_m in [os.path.join(cfg_dir, "mqtt.json"), os.path.join(cfg_dir, "mqtt_config.json")]:
            if os.path.isfile(cand_m):
                shutil.copyfile(cand_m, os.path.join(dest_harmony, "mqtt.json"))
                stats["mqtt"] = True
                break

    # 7. Network & BT stack keys
    if cfg_dir:
        for cand_net in [os.path.join(cfg_dir, "ethernet.conf"), os.path.join(cfg_dir, "hub_id")]:
            if os.path.isfile(cand_net):
                shutil.copyfile(cand_net, os.path.join(dest_harmony, os.path.basename(cand_net)))
        for cand_k in ["btstack_keys.tlv", "btstack_keys.bin"]:
            kf = os.path.join(cfg_dir, cand_k)
            if os.path.isfile(kf):
                shutil.copyfile(kf, os.path.join(dest_harmony, cand_k))

    return stats


def migrate_bundle_file(bundle_path: str, dest_harmony: str) -> Dict[str, Any]:
    """Import a JSON backup bundle into canonical v2.0 files."""
    os.makedirs(os.path.join(dest_harmony, "config"), exist_ok=True)
    with open(bundle_path, "r", encoding="utf-8") as f:
        bundle = json.load(f)

    stats: Dict[str, Any] = {"devices": 0, "activities": 0, "slot1_map": False, "slot2_map": False, "bt_remote_map": False, "mqtt": False, "settings": False}

    dev_data = bundle.get("devices")
    if dev_data:
        dev_obj = sanitize_devices_json(dev_data)
        # An old bundle's "bluetooth" section (bt-devices.json): its hosts go into devices.json
        merge_bt_device_hosts(dev_obj, bundle.get("bluetooth"))
        with open(os.path.join(dest_harmony, "config", "devices.json"), "w", encoding="utf-8") as f:
            json.dump(dev_obj, f, indent=2)
        stats["devices"] = len(dev_obj.get("devices", []))

    act_data = bundle.get("activities")
    id_map = {}
    if act_data:
        act_obj, id_map = sanitize_activities_json(act_data)
        with open(os.path.join(dest_harmony, "config", "activities.json"), "w", encoding="utf-8") as f:
            json.dump(act_obj, f, indent=2)
        stats["activities"] = len(act_obj.get("activities", []))

    set_data = bundle.get("settings")
    if set_data:
        with open(os.path.join(dest_harmony, "config", "settings.json"), "w", encoding="utf-8") as f:
            json.dump(set_data, f, indent=2)
        stats["settings"] = True

    mqtt_data = bundle.get("mqtt")
    if mqtt_data:
        with open(os.path.join(dest_harmony, "mqtt.json"), "w", encoding="utf-8") as f:
            json.dump(mqtt_data, f, indent=2)
        stats["mqtt"] = True

    # Slot 1 (RF)
    s1_data = bundle.get("remote_slot1_map") or bundle.get("elite_remote_map")
    if s1_data:
        if id_map:
            s1_data = remap_remote_map_activities(s1_data, id_map)
        with open(os.path.join(dest_harmony, "remote_slot1_map.json"), "w", encoding="utf-8") as f:
            json.dump(s1_data, f, indent=2)
        stats["slot1_map"] = True

    # Slot 2 (RF)
    s2_data = bundle.get("remote_slot2_map")
    if s2_data:
        if id_map:
            s2_data = remap_remote_map_activities(s2_data, id_map)
        with open(os.path.join(dest_harmony, "remote_slot2_map.json"), "w", encoding="utf-8") as f:
            json.dump(s2_data, f, indent=2)
        stats["slot2_map"] = True

    # Bluetooth Remote Map
    bt_map_data = bundle.get("bt_remote_map")
    if bt_map_data:
        if id_map:
            bt_map_data = remap_remote_map_activities(bt_map_data, id_map)
        with open(os.path.join(dest_harmony, "bt_remote_map.json"), "w", encoding="utf-8") as f:
            json.dump(bt_map_data, f, indent=2)
        stats["bt_remote_map"] = True

    net_data = bundle.get("network")
    if net_data:
        with open(os.path.join(dest_harmony, "ethernet.conf"), "w", encoding="utf-8") as f:
            f.write(net_data if isinstance(net_data, str) else json.dumps(net_data))

    wifi_data = bundle.get("wifi")
    if wifi_data:
        with open(os.path.join(dest_harmony, "wpa_supplicant.conf"), "w", encoding="utf-8") as f:
            f.write(wifi_data if isinstance(wifi_data, str) else json.dumps(wifi_data))

    bt_keys = bundle.get("btstack_keys")
    if bt_keys and isinstance(bt_keys, str):
        try:
            raw_keys = base64.b64decode(bt_keys)
            with open(os.path.join(dest_harmony, "btstack_keys.tlv"), "wb") as f:
                f.write(raw_keys)
            with open(os.path.join(dest_harmony, "btstack_keys.bin"), "wb") as f:
                f.write(raw_keys)
        except Exception:
            pass

    return stats


def migrate(src_path: str, dest_dir: str) -> Dict[str, Any]:
    """Migrate from source file/folder to destination harmony directory."""
    src = pathlib.Path(src_path).resolve()
    dest = pathlib.Path(dest_dir).resolve()
    dest.mkdir(parents=True, exist_ok=True)

    if src.is_file() and src.name.endswith(".tar.gz"):
        import tempfile
        tmp = tempfile.mkdtemp(prefix="harmony_tar_")
        try:
            with tarfile.open(src, "r:gz") as tar:
                for m in tar.getmembers():
                    if m.isfile() or m.isdir():
                        tar.extract(m, tmp)
            return migrate_directory(tmp, str(dest))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    elif src.is_file() and src.name.endswith(".json"):
        return migrate_bundle_file(str(src), str(dest))
    elif src.is_dir():
        return migrate_directory(str(src), str(dest))
    else:
        raise ValueError(f"Unsupported source format: {src}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Harmony Hub Configuration Migration Tool")
    parser.add_argument("--src", required=True, help="Source path (directory, tar.gz, or bundle.json)")
    parser.add_argument("--dest", required=True, help="Destination directory (e.g. /data/harmony or local test dir)")
    args = parser.parse_args()

    try:
        stats = migrate(args.src, args.dest)
    except Exception as e:
        print(f"[-] Migration failed: {e}")
        sys.exit(1)

    print(f"[+] Migration complete -> {args.dest}")
    print(f"    Devices: {stats.get('devices')}")
    print(f"    Activities: {stats.get('activities')}")
    print(f"    Settings: {stats.get('settings')}")
    print(f"    Remote Slot 1 (Elite): {stats.get('slot1_map')}")
    print(f"    Remote Slot 2 (Companion): {stats.get('slot2_map')}")
    print(f"    Bluetooth Remote Map: {stats.get('bt_remote_map')}")
    print(f"    MQTT: {stats.get('mqtt')}")
    for w in stats.get("warnings") or []:
        print(f"[!] {w}")

if __name__ == "__main__":
    main()
