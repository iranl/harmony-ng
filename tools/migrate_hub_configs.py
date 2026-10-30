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
- /data/harmony/bt-devices.json
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


def migrate_stock_devices(device_list_path: str, function_list_path: Optional[str] = None) -> Dict[str, Any]:
    """Convert stock Logitech DeviceList.json to canonical devices.json."""
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
        transport = int(d.get("Transport") or 1)

        is_bt = (transport == 32) or ("bluetooth" in disp_type.lower()) or ("bt" in disp_type.lower())
        is_mqtt = "mqtt" in disp_type.lower()
        clean_type = "BT" if is_bt else ("MQTT" if is_mqtt else "IR")

        cp = d.get("ControlPort")
        control_port = int(cp) if cp is not None else 7

        inter_device = int(d.get("InterDeviceDelay") or 500)
        inter_key = int(d.get("InterKeyDelay") or 100)
        hold_inter_key = int(d.get("HoldInterKeyDelay") or 200)
        default_inter = int(d.get("DefaultInterDeviceDelay") or 500)
        power_on_delay = int(d.get("PowerOnDelay") or 1500)

        bdaddr = d.get("BTAddress") or ""

        # Commands
        raw_cmds = item.get("Commands", [])
        clean_cmds = []
        has_connect, has_disconnect = False, False

        for idx, c in enumerate(raw_cmds):
            cname = c.get("Name") or ""
            if not cname:
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

        # Power sequences
        p_on = item.get("PowerOnActions") or []
        p_off = item.get("PowerOffActions") or []

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
            "isPowerAlwaysOn": False,
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


def migrate_stock_activities(activity_list_path: str) -> Tuple[Dict[str, Any], Dict[str, str]]:
    """Convert stock Logitech ActivityList.json to canonical activities.json with short IDs.

    Returns:
        (canonical_activities_dict, id_mapping_dict)
    """
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

        # Start sequence
        start_seq = a.get("startSequence") or a.get("StartSequence") or a.get("EnterActions") or []
        norm_start = []
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
                "order": int(s.get("Order") or s.get("ActionOrder") or idx)
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


def generate_default_remote_map(activities_obj: Dict[str, Any], devices_obj: Dict[str, Any], remote_name: str) -> Dict[str, Any]:
    """Generate canonical remote button map for Slot 1 (Elite) or Slot 2 (Companion)."""
    acts = activities_obj.get("activities", [])
    devs = {str(d.get("id")): d for d in devices_obj.get("devices", [])}

    act_mappings: Dict[str, Any] = {}

    for act in acts:
        aid = str(act.get("id"))
        dev_ids = act.get("deviceIds", [])
        act_devs = [devs[did] for did in dev_ids if did in devs]

        buttons: Dict[str, Any] = {}

        # Map standard TV / Audio / Media keys from participating devices
        standard_keys = [
            "VolumeUp", "VolumeDown", "Mute", "ChannelUp", "ChannelDown",
            "Play", "Pause", "Stop", "Rewind", "FastForward",
            "DirectionUp", "DirectionDown", "DirectionLeft", "DirectionRight", "OK",
            "Back", "Home", "Menu", "Exit", "Info"
        ]

        for skey in standard_keys:
            mapped = False
            for d in act_devs:
                cmds = d.get("commands", [])
                for c in cmds:
                    cname = c.get("name", "")
                    if cname.lower() == skey.lower():
                        buttons[skey] = {
                            "short": {"deviceId": str(d.get("id")), "command": cname},
                            "long": {"deviceId": str(d.get("id")), "command": cname}
                        }
                        mapped = True
                        break
                if mapped:
                    break

        act_mappings[aid] = {"buttons": buttons}

    return {
        "remote": remote_name,
        "activities": act_mappings
    }


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
    stats: Dict[str, Any] = {"devices": 0, "activities": 0, "slot1_map": False, "slot2_map": False, "bt_remote_map": False, "mqtt": False, "settings": False}

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
    if (not devices_obj or not devices_obj.get("devices")) and res_dir:
        dev_list = os.path.join(res_dir, "DeviceList.json")
        fn_list = os.path.join(res_dir, "FunctionList.json")
        devices_obj = migrate_stock_devices(dev_list, fn_list)

    if (not activities_obj or not activities_obj.get("activities")) and res_dir:
        act_list = os.path.join(res_dir, "ActivityList.json")
        activities_obj, id_map = migrate_stock_activities(act_list)

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
    elif activities_obj and devices_obj:
        slot1_obj = generate_default_remote_map(activities_obj, devices_obj, "Harmony Elite")

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
    elif activities_obj and devices_obj and res_dir:
        # Auto-generate RF companion map only on initial stock conversion
        slot2_obj = generate_default_remote_map(activities_obj, devices_obj, "Harmony Companion")

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

    # 6. BT devices
    if cfg_dir:
        for cand_b in [os.path.join(cfg_dir, "bt-devices.json"), os.path.join(cfg_dir, "bt_devices.json")]:
            if os.path.isfile(cand_b):
                shutil.copyfile(cand_b, os.path.join(dest_harmony, "bt-devices.json"))
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

    bt_devs = bundle.get("bluetooth")
    if bt_devs:
        with open(os.path.join(dest_harmony, "bt-devices.json"), "w", encoding="utf-8") as f:
            json.dump(bt_devs, f, indent=2)

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

if __name__ == "__main__":
    main()
