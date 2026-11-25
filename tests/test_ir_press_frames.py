#!/usr/bin/env python3
"""
An infrared press sends the frames a stock Hub sends (hw_action.c, ir_press_repeat_parts()).

Stock (irmanager.lua generateFromKeyCode, hal ir_send_thread) sends a press of a protocol code
as the start part, then N repeat parts, N = the device's PressMinRepeats from the Logitech
account (3 when the account has none). A keycode with an empty start group has no start part:
its frame is the repeat part, so a press sends it max(N, 1) times. A learned code goes out once.
The command's own "repeats" and the keycode's trailing ":3" do not count. Before, a tap sent the
start sequence only, whatever the device.

The daemon sends for real (dry run off) through the mock transmitter in real time, as in
tests/test_ir_hold.py, whose frame reader this test uses.

Checks:
 1. pressMinRepeats is kept by the loader and shown by GET /api/inventory, a device without it
    gets none; /api/device-save sets it, clamps it to 20, removes it with null and refuses text;
    the web UI's form post sets it and removes it with an empty field
 2. a tap of a "()(code)()" command: pressMinRepeats 1 -> 1 frame, 0 -> 1 frame, absent -> 3
 3. a tap of a "(code)(Repeat)()" command with pressMinRepeats 1: the frame + 1 repeat code
 4. a learned "F..." command: one send, also without pressMinRepeats
 5. a Sony command with pressMinRepeats 1: 3 single frames (Sony receivers need a repeat)
 6. a held key released at once still sends start + N repeat parts (N 3, the field absent)
 7. an RC5 "Toggle" code goes out with its toggle bit (hw_action.c, rc5_toggle_take()): every
    frame of one press has the same bit, the next press of the device the other one, the rest
    of the code unchanged; a held key keeps its press's bit; an RC5 code without "Toggle" goes
    out as stored each time
"""

import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402
from test_ir_hold import Keys, Tx, kinds  # noqa: E402

PORT = 8958
failed = 0

TV_CODE = "G:GoVideoO1 32 Bit:()(0x0707FD02)():3"
XBOX_CODE = "G:Toshiba 32 Bit:(0x011B946B)(Repeat)():3"
SONY_CODE = "G:Sony 12 Bit:()(0x481)():3"
# a learned NEC frame: header, 32 bits, end mark, 40 ms gap (F = carrier in Hz, hex)
RC5_VALUE = 0x140D
RC5_TOGGLE_CODE = "G:Philips RC5 13 Bit Toggle:()(0x%04X)():3" % RC5_VALUE
RC5_PLAIN_CODE = "G:Philips RC5 13 Bit:()(0x%04X)():3" % RC5_VALUE
RC5_TOGGLE_BIT = 0x0800   # bit 11 of the 13 in the code
LEARNED = "F9470P2328S1194" + "".join("P230S%X" % (0x668 if (0x20DF10EF >> (31 - i)) & 1 else 0x230)
                                       for i in range(32)) + "P230S9C40"


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def device(dev_id, name, code, press_min_repeats=None):
    d = {"id": dev_id, "name": name, "type": "IR", "manufacturer": "Example", "model": name,
         "controlPort": 7, "interDeviceDelay": 0, "interKeyDelay": 0, "holdInterKeyDelay": 0,
         "commands": [{"name": "Key", "keyCode": code, "repeats": 3}]}
    if press_min_repeats is not None:
        d["pressMinRepeats"] = press_min_repeats
    return d


def devices():
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        d = json.load(f)
    d["devices"] += [
        device("21", "TvOne", TV_CODE, 1),
        device("22", "TvZero", TV_CODE, 0),
        device("23", "TvUnset", TV_CODE),
        device("24", "XboxOne", XBOX_CODE, 1),
        device("25", "Learned", LEARNED),
        device("26", "SonyOne", SONY_CODE, 1),
        device("27", "XboxUnset", XBOX_CODE),
        device("28", "TextCount", TV_CODE, "2"),
        device("29", "BadCount", TV_CODE, "many"),
        device("30", "BigCount", TV_CODE, 99),
        device("31", "Rc5Toggle", RC5_TOGGLE_CODE),
        device("32", "Rc5Plain", RC5_PLAIN_CODE),
    ]
    return d


def inventory(hub):
    st, r = hub.http_get("/api/inventory")
    return {str(d.get("id")): d for d in (r.get("devices") or [])} if st == 200 else {}


def test_field(hub):
    print("[*] 1. the pressMinRepeats field")
    inv = inventory(hub)
    check(inv.get("21", {}).get("pressMinRepeats") == 1 and inv.get("22", {}).get("pressMinRepeats") == 0,
          "kept by the loader (1 and 0)", str({k: inv.get(k, {}).get("pressMinRepeats") for k in ("21", "22")}))
    check("pressMinRepeats" not in inv.get("23", {"pressMinRepeats": "?"}), "none added to a device without it")
    check(inv.get("28", {}).get("pressMinRepeats") == 2, "numeric text loaded as a number")
    check("pressMinRepeats" not in inv.get("29", {"pressMinRepeats": "?"}), "a value that is no number dropped")
    check(inv.get("30", {}).get("pressMinRepeats") == 20, "99 lowered to 20")

    st, _ = hub.http_post("/api/device-save", {"deviceId": "23", "pressMinRepeats": 5})
    check(st == 200 and inventory(hub).get("23", {}).get("pressMinRepeats") == 5, "device-save sets it", str(st))
    st, _ = hub.http_post("/api/device-save", {"deviceId": "23", "name": "TvUnset"})
    check(st == 200 and inventory(hub).get("23", {}).get("pressMinRepeats") == 5, "a save without it keeps it")
    st, _ = hub.http_post("/api/device-save", {"deviceId": "23", "pressMinRepeats": 50})
    check(st == 200 and inventory(hub).get("23", {}).get("pressMinRepeats") == 20, "device-save lowers 50 to 20")
    st, _ = hub.http_post("/api/device-save", {"deviceId": "23", "pressMinRepeats": "abc"})
    check(st == 400 and inventory(hub).get("23", {}).get("pressMinRepeats") == 20, "device-save refuses text (400)", str(st))
    st, _ = hub.http_post("/api/device-save", {"deviceId": "23", "pressMinRepeats": None})
    check(st == 200 and "pressMinRepeats" not in inventory(hub).get("23", {"pressMinRepeats": "?"}),
          "device-save with null removes it", str(st))
    # the legacy web UI posts a form (/ir/device): a number as text sets it, an empty field removes it
    form = {"Content-Type": "application/x-www-form-urlencoded"}
    st, _ = hub.http_post("/ir/device", None, headers=form, raw=b"deviceId=23&pressMinRepeats=4")
    check(st == 200 and inventory(hub).get("23", {}).get("pressMinRepeats") == 4, "form post sets it", str(st))
    st, _ = hub.http_post("/ir/device", None, headers=form, raw=b"deviceId=23&pressMinRepeats=")
    check(st == 200 and "pressMinRepeats" not in inventory(hub).get("23", {"pressMinRepeats": "?"}),
          "form post with an empty field removes it", str(st))


def tap(hub, tx, dev):
    tx.mark()
    st, r = hub.http_post("/api/ir-send", {"deviceId": dev, "command": "Key"}, timeout=10)
    fr = tx.wait_quiet()
    return st, fr


def test_taps(hub, tx):
    print("[*] 2-5. taps")
    st, fr = tap(hub, tx, "21")
    check(st == 200 and kinds(fr) == "S", "()(code)() with pressMinRepeats 1: one frame (%s)" % kinds(fr))
    st, fr = tap(hub, tx, "22")
    check(st == 200 and kinds(fr) == "S", "()(code)() with pressMinRepeats 0: one frame (%s)" % kinds(fr))
    st, fr = tap(hub, tx, "23")
    check(st == 200 and kinds(fr) == "SSS" and len(set(f.code() for f in fr)) == 1,
          "()(code)() without pressMinRepeats: the frame 3 times (%s)" % kinds(fr))
    st, fr = tap(hub, tx, "24")
    check(st == 200 and kinds(fr) == "Sr" and fr[0].code() == 0x011B946B,
          "(code)(Repeat)() with pressMinRepeats 1: the frame and one repeat code (%s)" % kinds(fr))
    st, fr = tap(hub, tx, "25")
    check(st == 200 and len(fr) == 1 and fr[0].code() == 0x20DF10EF,
          "a learned code without pressMinRepeats: sent once (%d send(s))" % len(fr))
    st, fr = tap(hub, tx, "26")
    marks = [len(f.marks) for f in fr]
    check(st == 200 and marks == [13, 13, 13],
          "Sony with pressMinRepeats 1: three single frames (marks per stream %s)" % marks)


def test_quick_hold(hub, tx, keys):
    print("[*] 6. a held key released at once")
    tx.mark()
    keys.play([(0, "press", "Key", "27"), (20, "release", "Key", "27")])
    fr = tx.wait_quiet()
    check(kinds(fr) == "Srrr", "(code)(Repeat)() without pressMinRepeats: the frame and 3 repeat codes (%s)" % kinds(fr))


def rc5_value(frame):
    """The 13 bits of an RC5 frame as the keycode has them (None: no RC5 frame). On the wire it
    is 14 bits of two 889 us halves each, a 0 a mark then a space, a 1 a space then a mark; the
    first bit is the start bit 1, whose space half is the idle line before the frame."""
    halves = [0]
    for i, mark in enumerate(frame.marks):
        halves += [1] * int(round(len(mark) * frame.us_per_bit / 889.0))
        if i < len(frame.spaces):
            halves += [0] * int(round(len(frame.spaces[i]) * frame.us_per_bit / 889.0))
    if len(halves) == 27:
        halves.append(0)   # a last bit 0 ends on a space, which the frame reader drops
    if len(halves) != 28:
        return None
    v = 0
    for first, second in zip(halves[0::2], halves[1::2]):
        if first == second:
            return None
        v = (v << 1) | second
    return v & 0x1FFF if v & 0x2000 else None


def test_rc5_toggle(hub, tx, keys):
    print("[*] 7. the RC5 toggle bit on the wire")
    sent = []
    for _ in range(3):
        st, fr = tap(hub, tx, "31")
        sent.append([rc5_value(f) for f in fr])
        check(st == 200 and len(fr) == 3, "a tap of a Toggle code: 3 frames (%d)" % len(fr), str(st))
    hexes = [["None" if v is None else "0x%04X" % v for v in vals] for vals in sent]
    check(all(len(set(vals)) == 1 and vals[0] is not None for vals in sent),
          "every frame of one tap is the same RC5 frame", str(hexes))
    first = [vals[0] if vals else None for vals in sent]
    check(first == [RC5_VALUE, RC5_VALUE ^ RC5_TOGGLE_BIT, RC5_VALUE],
          "the taps go out as stored, with bit 11 flipped, as stored", str(hexes))

    # the fourth press of the device, held: the press and its repeats have one bit
    tx.mark()
    keys.play([(0, "press", "Key", "31"), (400, "release", "Key", "31")])
    held = [rc5_value(f) for f in tx.wait_quiet()]
    check(len(held) >= 3 and set(held) == {RC5_VALUE ^ RC5_TOGGLE_BIT},
          "a held key: every frame with the press's bit (%d frames)" % len(held),
          str(["None" if v is None else "0x%04X" % v for v in held]))
    time.sleep(0.3)
    st, fr = tap(hub, tx, "31")
    check(st == 200 and [rc5_value(f) for f in fr] == [RC5_VALUE] * 3, "the tap after the hold: as stored again")

    plain = []
    for _ in range(2):
        st, fr = tap(hub, tx, "32")
        plain.append([rc5_value(f) for f in fr])
    check(plain == [[RC5_VALUE] * 3] * 2, "a code without Toggle: as stored both times", str(plain))


def main():
    with tempfile.TemporaryDirectory(prefix="irpress_", dir="/tmp" if os.path.isdir("/tmp") else None) as tmp:
        sock_path = os.path.join(tmp, "keys.sock")
        log_path = os.path.join(tmp, "daemon.log")
        with HubHarness(port=PORT, log_path=log_path, args=["--key-socket", sock_path],
                        extra_config_files={"devices.json": devices()}) as hub:
            with open(os.path.join(hub.mock_dir, "i2s_tx_realtime"), "w") as f:
                f.write("1")
            st, _ = hub.http_post("/api/settings", {"dry_run": False})
            check(st == 200 and os.path.exists(sock_path), "daemon up: real sends through the mock transmitter")
            tx = Tx(hub)
            keys = Keys(sock_path)
            test_field(hub)
            test_taps(hub, tx)
            time.sleep(0.3)
            test_quick_hold(hub, tx, keys)
            time.sleep(0.3)
            test_rc5_toggle(hub, tx, keys)
    if failed:
        print("[-] test_ir_press_frames: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_ir_press_frames passed")


if __name__ == "__main__":
    main()
