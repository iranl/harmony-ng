#!/usr/bin/env python3
"""
test_migrate_stock_config.py - Stock Logitech config migration (tools/migrate_hub_configs.py)

Builds a small stock /data/resources tree (DeviceList, ActivityList, MapList) and checks
device transports and power, activity inputs, and the remote maps generated from the stock
ButtonMaps and roles.
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import migrate_hub_configs as mig  # noqa: E402

TV, SOUNDBAR, PLAYER = 75563901, 67964582, 68073190
WATCH, LISTEN = 51097176, 43427356


def ir(name, transport=1):
    return {"Name": name, "KeyCode": f"G:Test 32 Bit:()(0x{abs(hash(name)) % 0xFFFFFFFF:08X})():3",
            "Raw": "", "TransportType": transport}


def press(*names):
    return [{"__type": "IRPressAction", "IRCommandName": n, "Order": i, "Duration": None} for i, n in enumerate(names)]


DEVICE_LIST = {"DevicesWithFeatures": [
    {"Device": {"Id-": TV, "Name": "TV", "Transport": 33, "ControlPort": 7},
     "Commands": [ir(n) for n in ("PowerOn", "PowerOff", "Exit", "InputTv", "InputHdmi2", "Mute", "VolumeUp",
                                  "VolumeDown", "ChannelUp", "Return", "Select", "SmartHub")]
     + [ir("HIDDirectionUp", 32)],
     "DeviceFeatures": [
         {"__type": "InputFeature", "InputType": 1, "InputDelay": 0,
          "Inputs": [{"InputName": "TV", "Actions": press("Exit", "InputTv")},
                     {"InputName": "HDMI 2", "Actions": press("InputHdmi2", "InputHdmi2")}]},
         {"__type": "PowerFeature", "PowerTypeId": 1, "PowerOnDelay": 5000, "IsPowerAlwaysOn": False,
          "PowerOnActions": press("PowerOn"), "PowerOffActions": press("PowerOff"), "PowerToggleActions": None}]},
    {"Device": {"Id-": SOUNDBAR, "Name": "Soundbar", "Transport": 1, "ControlPort": 7},
     "Commands": [ir(n) for n in ("PowerToggle", "Source", "Mute", "VolumeUp", "VolumeDown")],
     "DeviceFeatures": [
         {"__type": "InputFeature", "InputType": 4, "InputDelay": 1000,
          "Inputs": [{"InputName": "eARC", "Actions": None}]},
         {"__type": "PowerFeature", "PowerTypeId": 2, "PowerOnDelay": 4000, "IsPowerAlwaysOn": False,
          "PowerOnActions": None, "PowerOffActions": None, "PowerToggleActions": press("PowerToggle")}]},
    {"Device": {"Id-": PLAYER, "Name": "Player", "Transport": 32, "ControlPort": 7},
     "Commands": [ir(n, 32) for n in ("Home", "Sleep", "Select", "Back", "DirectionUp", "Play")],
     "DeviceFeatures": [
         {"__type": "PowerFeature", "PowerTypeId": 1, "PowerOnDelay": 1500,
          "PowerOnActions": press("Home"), "PowerOffActions": press("Sleep")}]},
]}

ACTIVITY_LIST = {"Activities": [
    {"Id-": WATCH, "Name": "Watch TV", "Type": "VirtualTelevisionN", "ActivityOrder": 1, "EnterActions": [],
     "LeaveActions": [],
     "Roles": [
         {"__type": "DisplayActivityRole", "DeviceId-": TV, "SelectedInput": {"Name": "TV"}, "PowerOnOrder": 1},
         {"__type": "VolumeActivityRole", "DeviceId-": SOUNDBAR, "SelectedInput": {"Name": "eARC"},
          "PowerOnOrder": 2}]},
    {"Id-": LISTEN, "Name": "Player", "Type": "VirtualGeneric", "ActivityOrder": 2,
     "EnterActions": [{"__type": "CommandActivityAction", "DeviceId-": PLAYER, "CommandName": "Home",
                       "ActionOrder": 5}],
     "LeaveActions": [],
     "Roles": [
         {"__type": "DisplayActivityRole", "DeviceId-": TV, "SelectedInput": {"Name": "HDMI 2"}, "PowerOnOrder": 1},
         {"__type": "PlayMovieActivityRole", "DeviceId-": PLAYER, "SelectedInput": None, "PowerOnOrder": 3},
         {"__type": "VolumeActivityRole", "DeviceId-": TV, "SelectedInput": {"Name": "HDMI 2"},
          "PowerOnOrder": 1}]},
]}


def hard(key, dev, cmd, long_cmd=None):
    b = {"__type": "HardRemoteButton", "ButtonKey": key,
         "ButtonAction": {"__type": "ButtonCommandAction", "DeviceId-": dev, "CommandName": cmd}}
    if long_cmd:
        b["ButtonLongPressAction"] = {"__type": "ButtonCommandAction", "DeviceId-": dev, "CommandName": long_cmd}
    return b


def soft(index, dev, cmd, group=0):
    return {"__type": "SoftRemoteButton", "TextOnRemote": cmd, "FunctionGroupType": group,
            "MenuItem": {"IndexInMenu": index},
            "ButtonAction": {"__type": "ButtonCommandAction", "DeviceId-": dev, "CommandName": cmd}}


MAP_LIST = {"ButtonMaps": [
    # Only the "Watch TV" activity has a stock map; "Player" must get a role-based default
    {"ButtonMapIdentifier": f"16417Activity{WATCH}", "RemoteId-": 1234567, "Buttons": [
        hard("VolumeMute", SOUNDBAR, "Mute"),
        hard("PrevChannel", TV, "Return"),
        hard("Menu", TV, "SmartHub", long_cmd="Exit"),
        hard("Select", TV, "Select"),
        hard("TelevisionPower", TV, "PowerOn"),
        soft(1, TV, "Exit"),
        soft(0, TV, "SmartHub"),
        soft(0, TV, "Mute", group=2),
        {"__type": "GestureRemoteButton", "ButtonKey": "SwipeUp",
         "ButtonAction": {"__type": "ButtonCommandAction", "DeviceId-": SOUNDBAR, "CommandName": "VolumeUp"}},
    ]},
    {"ButtonMapIdentifier": f"16417Device{TV}", "RemoteId-": 1234567, "Buttons": [
        hard("PrevChannel", TV, "Return"),
        hard("VolumeMute", TV, "Mute"),
    ]},
]}


class StockMigrationTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        res = root / "src" / "data" / "resources"
        res.mkdir(parents=True)
        (res / "DeviceList.json").write_text(json.dumps(DEVICE_LIST))
        (res / "ActivityList.json").write_text(json.dumps(ACTIVITY_LIST))
        (res / "MapList.json").write_text(json.dumps(MAP_LIST))
        dest = root / "dest"
        cls.stats = mig.migrate(str(root / "src"), str(dest))
        cls.devices = {d["name"]: d for d in json.loads((dest / "config" / "devices.json").read_text())["devices"]}
        cls.activities = {a["name"]: a for a in
                          json.loads((dest / "config" / "activities.json").read_text())["activities"]}
        cls.slot1 = json.loads((dest / "remote_slot1_map.json").read_text())
        cls.slot2 = json.loads((dest / "remote_slot2_map.json").read_text())

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def warned(self, text):
        return any(text in w for w in self.stats["warnings"])

    def test_short_device_ids(self):
        self.assertEqual([self.devices[n]["id"] for n in ("TV", "Soundbar", "Player")], ["1", "2", "3"])

    def test_transport_bitmask(self):
        tv = self.devices["TV"]
        self.assertEqual(tv["type"], "IR")
        names = [c["name"] for c in tv["commands"]]
        self.assertNotIn("HIDDirectionUp", names)
        self.assertIn("PowerOn", names)
        self.assertTrue(self.warned("1 Bluetooth commands not migrated"))
        self.assertEqual(self.devices["Player"]["type"], "BT")

    def test_power_feature(self):
        tv = self.devices["TV"]
        self.assertEqual(tv["powerOnDelay"], 5000)
        self.assertEqual([s["command"] for s in tv["powerOnSequence"]], ["PowerOn"])
        self.assertEqual([s["command"] for s in tv["powerOffSequence"]], ["PowerOff"])
        sb = self.devices["Soundbar"]
        self.assertEqual(sb["powerOnDelay"], 4000)
        self.assertEqual(sb["powerOnSequence"], [])
        self.assertEqual(sb["powerOffSequence"], [])
        self.assertEqual([s["command"] for s in self.devices["Player"]["powerOffSequence"]], ["Sleep"])

    def test_activity_inputs(self):
        watch = self.activities["Watch TV"]
        self.assertEqual(watch["deviceIds"], ["1", "2"])
        self.assertEqual([(s["deviceId"], s["command"]) for s in watch["startSequence"]],
                         [("1", "Exit"), ("1", "InputTv")])
        self.assertTrue(self.warned("input 'eARC' on 'Soundbar' not migrated"))
        player = self.activities["Player"]
        self.assertEqual([(s["deviceId"], s["command"]) for s in player["startSequence"]],
                         [("1", "InputHdmi2"), ("1", "InputHdmi2"), ("3", "Home")])
        self.assertEqual([s["order"] for s in player["startSequence"]], [0, 1, 2])

    def test_stock_activity_map(self):
        watch_id = self.activities["Watch TV"]["id"]
        m = self.slot1["activities"][watch_id]
        self.assertEqual(m["VolumeMute"], {"type": "command", "action": "device_cmd", "deviceId": "2",
                                           "command": "Mute"})
        self.assertEqual(m["Back"]["command"], "Return")
        self.assertEqual(m["Menu"]["command"], "SmartHub")
        self.assertNotIn("TelevisionPower", m)
        self.assertTrue(self.warned("stock key 'TelevisionPower' has no hub_rf button name"))
        self.assertEqual([b["label"] for b in m["screen_buttons"]], ["SmartHub", "Exit", "Mute"])
        self.assertTrue(self.warned("1 gestures, 1 long/double presses"))

    def test_stock_device_map(self):
        self.assertEqual(self.slot1["devices"]["1"]["Back"]["command"], "Return")
        self.assertEqual(self.slot1["devices"]["1"]["VolumeMute"]["command"], "Mute")

    def test_default_map_uses_roles(self):
        player_id = self.activities["Player"]["id"]
        m = self.slot1["activities"][player_id]
        self.assertEqual((m["VolumeUp"]["deviceId"], m["VolumeUp"]["command"]), ("1", "VolumeUp"))
        self.assertEqual((m["Select"]["deviceId"], m["Select"]["command"]), ("3", "Select"))
        self.assertEqual((m["Back"]["deviceId"], m["Back"]["command"]), ("3", "Back"))
        companion = self.slot2["activities"][self.activities["Watch TV"]["id"]]
        self.assertEqual((companion["VolumeMute"]["deviceId"], companion["VolumeMute"]["command"]), ("2", "Mute"))
        self.assertEqual((companion["Back"]["deviceId"], companion["Back"]["command"]), ("1", "Return"))

    def test_convert_hid_code(self):
        self.assertEqual(mig.convert_hid_code("HID:07:0027"), "HID:07:0027")
        self.assertEqual(mig.convert_hid_code("G:HID(0x07, 0x0028)"), "HID:07:0028")
        self.assertEqual(mig.convert_hid_code("G:HID 16 Bit:()(07000027)():1"), "HID:07:0027")
        self.assertEqual(mig.convert_hid_code("G:HID 16 Bit:()(0C000041)():1"), "HID:0C:0041")


if __name__ == "__main__":
    unittest.main()
