#!/usr/bin/env python3
"""
A held infrared key repeats until its release (hw_action.c, the held-key player).

A press starts the code and returns: one thread of hub_daemon sends the code's start sequence,
then its repeat sequence again and again with the code's own timing, until the release, until
no hold came for HW_IR_HOLD_DEADLINE_MS (600 ms), or until another command needs the
transmitter. A hold is only the heartbeat of the key that plays. Before, the press was one
blocking send, every hold another one (the remote sent them after 450 ms and then every
150 ms) and the release nothing: the device saw separate bursts, the first gap 0.45 s long.

The daemon sends for real (dry run off) through the mock transmitter in real time
(<mock>/i2s_tx_realtime): <mock>/i2s_tx holds every stream that went out and <mock>/i2s_tx_log
its start time and length. Keys come in on the key channel, as hub_rf sends them.

Checks:
 1. held key (press, hold every 300 ms, release at 1.5 s): start sequence once, then only the
    repeat sequence, back to back for 1.5 s, the last frame within one frame of the release;
    the press does not block the key thread (the release is taken while the frame is on air)
 2. quick tap (release 30 ms after the press): exactly the start sequence (the TV here has
    pressMinRepeats 0; tests/test_ir_press_frames.py checks the repeat parts of a press)
 3. lost release (press, two holds, nothing): stops by itself 600 ms after the last hold
 4. old remote timing (first hold after 450 ms, then every 150 ms): no stop in between
 5. second key pressed while the first is held: the first ends after its frame, the second
    starts at once, no stream overlaps another; the late release of the first is ignored
 6. a tap, a list of commands and the steps of an activity start send one start sequence
    each, as before; a tap while a key is held ends it, and holds for a key that does not play
    send nothing
 7. hold_secs of the send_command action (WebSocket, MQTT) holds the command for that long
 8. a code without a repeat sequence repeats its whole frame at the frame's own length; an NEC
    type code ("Toshiba 32 Bit") repeats its 108 ms repeat code; a Sony code sends single
    frames, at least three for a press
 9. capture mode on: light read during the whole hold gives no ir_received event; the next
    real press after the release does
"""

import json
import os
import re
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402
from ir_rx_signal import ir_rx_bitstream, nec_timings  # noqa: E402

PORT = 8951
DEADLINE_MS = 600        # HW_IR_HOLD_DEADLINE_MS
MARGIN_MS = 45           # scheduling slack of the test host between two frames
failed = 0

# The sample TV's codes are Pronto with a start sequence (an NEC frame: 34 marks) and a repeat
# sequence (the NEC repeat code: 2 marks, 108.9 ms with its space)
TV = "1"
PRONTO_REPEAT_MS = (0x0157 + 0x0056 + 0x0016 + 0x0E6C) * 0x6D * 0.241246 / 1000.0
# A code without a repeat sequence: the whole frame repeats, 9 ms header + 32 bits + end mark
# + the encoder's 45 ms gap
WHOLE_CODE = 0xE0E0E01F
WHOLE_MS = (4500 + 4500 + sum(560 + (1690 if (WHOLE_CODE >> i) & 1 else 560) for i in range(32)) + 560 + 45000) / 1000.0
NEC_REPEAT_MS = (8990 + 2230 + 568 + 96077) / 1000.0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def devices():
    with open(os.path.join(SAMPLE_CONFIG, "devices.json")) as f:
        d = json.load(f)
    d["devices"].append({
        "id": "9", "name": "Amp", "type": "Amplifier", "manufacturer": "Example", "model": "A-1",
        "controlPort": 7, "interDeviceDelay": 0, "interKeyDelay": 0, "holdInterKeyDelay": 0,
        "commands": [
            {"name": "Whole", "keyCode": "G:GoVideoO1 32 Bit:()(0x%08X)():1" % WHOLE_CODE, "repeats": 1},
            {"name": "Nec", "keyCode": "G:Toshiba 32 Bit:()(0x20DF40BF)():1", "repeats": 1},
            {"name": "Sony", "keyCode": "G:Sony 12 Bit:()(0x481)():3", "repeats": 3},
        ],
    })
    for dev in d["devices"]:
        if dev["id"] == TV:   # the test times the keys itself; a press is its start sequence only
            dev.update({"interDeviceDelay": 0, "interKeyDelay": 0, "holdInterKeyDelay": 0, "pressMinRepeats": 0})
    return d


def now_us():
    return int(time.monotonic() * 1e6)   # CLOCK_MONOTONIC, as the daemon and the mock


class Frame:
    def __init__(self, start_us, dur_us, data):
        self.start = start_us
        self.dur = dur_us
        self.end = start_us + dur_us
        # the stream as built: bytes of every 16-bit word swapped, a mark is 1 bits, MSB first
        raw = bytes(data[i ^ 1] for i in range(len(data)))
        bits = bin(int.from_bytes(raw, "big"))[2:].zfill(len(raw) * 8)
        self.us_per_bit = dur_us / float(len(bits))
        body = bits.strip("0")
        # inside a mark the carrier is dark for half a period (6 bits); 13 dark bits are a space
        self.marks = [m for m in re.split("0{13,}", body) if m]
        self.spaces = re.findall("0{13,}", body)

    def code(self):
        """The 32 bits of an NEC type frame (header mark, 32 bit marks, end mark)"""
        if len(self.marks) != 34:
            return None
        v = 0
        for sp in self.spaces[1:33]:
            v = (v << 1) | (1 if len(sp) * self.us_per_bit > 1100 else 0)
        return v


class Tx:
    """The mock transmitter's output since the last mark()"""

    def __init__(self, hub):
        self.log = os.path.join(hub.mock_dir, "i2s_tx_log")
        self.raw = os.path.join(hub.mock_dir, "i2s_tx")
        self.lines = 0
        self.offset = 0

    def _read(self):
        lines = []
        if os.path.exists(self.log):
            with open(self.log) as f:
                lines = [ln.split() for ln in f.read().splitlines() if ln.strip()]
        data = b""
        if os.path.exists(self.raw):
            with open(self.raw, "rb") as f:
                data = f.read()
        return lines, data

    def mark(self):
        lines, data = self._read()
        self.lines, self.offset = len(lines), len(data)

    def frames(self):
        lines, data = self._read()
        out, off = [], self.offset
        for ln in lines[self.lines:]:
            start, nbytes, dur = int(ln[0]), int(ln[1]), int(ln[2])
            out.append(Frame(start, dur, data[off:off + nbytes]))
            off += nbytes
        return out

    def wait_quiet(self, quiet=0.45, timeout=8.0):
        """Returns the frames once nothing new went out for `quiet` seconds"""
        t_end = time.time() + timeout
        last, last_change = -1, time.time()
        while time.time() < t_end:
            n = len(self._read()[0])
            if n != last:
                last, last_change = n, time.time()
            elif time.time() - last_change >= quiet:
                break
            time.sleep(0.02)
        return self.frames()


class Keys:
    """Key events on the key channel, as hub_rf sends them"""

    def __init__(self, path):
        self.path = path
        self.seq = 0

    def send(self, action, command, dev=TV):
        self.seq += 1
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            s.sendto(json.dumps({"seq": self.seq, "ts": int(time.monotonic() * 1000), "slot": 1, "button": command,
                                 "action": action, "deviceId": dev, "command": command}).encode(), self.path)
        finally:
            s.close()

    def play(self, events):
        """events: (ms after the start, action, command[, device]); returns the start (us)"""
        t0 = time.monotonic()
        for ev in events:
            delay = t0 + ev[0] / 1000.0 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            self.send(ev[1], ev[2], ev[3] if len(ev) > 3 else TV)
        return int(t0 * 1e6)


def kinds(frames):
    return "".join("S" if len(f.marks) == 34 else ("r" if len(f.marks) == 2 else "?") for f in frames)


def max_start_gap_ms(frames):
    return max([(b.start - a.start) / 1000.0 for a, b in zip(frames, frames[1:])] or [0.0])


def overlaps(frames):
    return [(i, (a.end - b.start) / 1000.0) for i, (a, b) in enumerate(zip(frames, frames[1:])) if b.start < a.end - 1000]


def median(values):
    v = sorted(values)
    return v[len(v) // 2] if v else 0.0


def test_held_key(hub, tx, keys, ws):
    print("[*] 1. held key: press, hold every 300 ms, release at 1.5 s")
    tx.mark()
    ws.drain(0.2)
    t0 = keys.play([(0, "press", "VolumeUp"), (300, "hold", "VolumeUp"), (600, "hold", "VolumeUp"),
                    (900, "hold", "VolumeUp"), (1200, "hold", "VolumeUp"), (1500, "release", "VolumeUp")])
    fr = tx.wait_quiet()
    k = kinds(fr)
    check(len(fr) >= 12 and k[0] == "S" and set(k[1:]) == {"r"},
          "the start sequence once, then only the repeat sequence (%s)" % k)
    if not fr:
        return
    check((fr[0].start - t0) / 1000.0 < MARGIN_MS, "the first frame starts at the press (%.0f ms)" % ((fr[0].start - t0) / 1000.0))
    gap = max_start_gap_ms(fr)
    check(gap <= PRONTO_REPEAT_MS + MARGIN_MS,
          "frames back to back: no start-to-start time over the repeat period %.1f ms + %d ms (max %.1f ms)"
          % (PRONTO_REPEAT_MS, MARGIN_MS, gap))
    release_ms = 1500.0
    last_start = (fr[-1].start - t0) / 1000.0
    last_end = (fr[-1].end - t0) / 1000.0
    check(last_end >= release_ms - MARGIN_MS, "frames go on until the release (last ends at %.0f ms)" % last_end)
    check(last_start <= release_ms + MARGIN_MS, "nothing starts after the release (last starts at %.0f ms)" % last_start)
    check(not overlaps(fr), "no stream overlaps the next", str(overlaps(fr)))
    rep = [(b.start - a.start) / 1000.0 for a, b in zip(fr[1:], fr[2:])]
    check(PRONTO_REPEAT_MS - 1.5 <= median(rep) <= PRONTO_REPEAT_MS + 10,
          "the repeat sequence at the code's own period %.1f ms (median %.1f ms)" % (PRONTO_REPEAT_MS, median(rep)))


def test_press_does_not_block(hub, tx, keys, ws):
    print("[*] 2. quick tap: release 30 ms after the press")
    tx.mark()
    ws.drain(0.3)
    t_press = time.monotonic()
    keys.send("press", "VolumeUp")
    time.sleep(0.03)
    keys.send("release", "VolumeUp")
    t_rel_event = None
    t_end = time.time() + 2.0
    while time.time() < t_end:
        m = ws.recv_json(0.5)
        if m and m.get("type") == "button" and m.get("action") == "release":
            t_rel_event = time.monotonic()
            break
    fr = tx.wait_quiet()
    check(kinds(fr) == "S", "exactly the start sequence went out (%s)" % kinds(fr))
    took = (t_rel_event - t_press) * 1000.0 if t_rel_event else -1
    check(t_rel_event is not None and fr and took < 0.8 * fr[0].dur / 1000.0,
          "the release is taken while the frame is on air (%.0f ms after the press; the frame lasts %.0f ms)"
          % (took, fr[0].dur / 1000.0 if fr else 0))


def test_lost_release(hub, tx, keys, log_path):
    print("[*] 3. lost release: press, two holds, then nothing")
    tx.mark()
    t0 = keys.play([(0, "press", "VolumeUp"), (300, "hold", "VolumeUp"), (600, "hold", "VolumeUp")])
    fr = tx.wait_quiet(quiet=0.6)
    if not fr:
        check(False, "frames went out")
        return
    stop_ms = 600 + DEADLINE_MS
    last_start = (fr[-1].start - t0) / 1000.0
    last_end = (fr[-1].end - t0) / 1000.0
    check(last_end >= stop_ms - MARGIN_MS,
          "it goes on for %d ms after the last hold (last frame ends at %.0f ms)" % (DEADLINE_MS, last_end))
    check(last_start <= stop_ms + MARGIN_MS,
          "it stops by itself: nothing starts after %d ms (last starts at %.0f ms)" % (stop_ms, last_start))
    check(max_start_gap_ms(fr) <= PRONTO_REPEAT_MS + MARGIN_MS and kinds(fr)[1:].count("r") == len(fr) - 1,
          "back to back until then (%s)" % kinds(fr))
    with open(log_path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    check("no hold or release for %d ms" % DEADLINE_MS in text, "the stop is logged")


def test_old_remote_timing(hub, tx, keys):
    print("[*] 4. old remote timing: first hold after 450 ms, then every 150 ms")
    tx.mark()
    events = [(0, "press", "VolumeUp")] + [(t, "hold", "VolumeUp") for t in range(450, 1500, 150)] + [(1500, "release", "VolumeUp")]
    t0 = keys.play(events)
    fr = tx.wait_quiet()
    gap = max_start_gap_ms(fr)
    k = kinds(fr)
    check(len(fr) >= 12 and gap <= PRONTO_REPEAT_MS + MARGIN_MS,
          "no stop in between: %d frames, max start-to-start %.1f ms" % (len(fr), gap))
    check(k[:1] == "S" and set(k[1:]) == {"r"}, "one start sequence, then repeats (%s)" % k)
    if fr:
        check((fr[-1].end - t0) / 1000.0 >= 1500 - MARGIN_MS, "until the release (last ends at %.0f ms)" % ((fr[-1].end - t0) / 1000.0))


def test_second_key(hub, tx, keys):
    print("[*] 5. a second key pressed while the first is held")
    tx.mark()
    t0 = keys.play([(0, "press", "VolumeUp"), (300, "hold", "VolumeUp"), (500, "press", "VolumeDown"),
                    (800, "hold", "VolumeDown"), (900, "release", "VolumeUp"), (1100, "hold", "VolumeDown"),
                    (1300, "release", "VolumeDown")])
    fr = tx.wait_quiet()
    k = kinds(fr)
    starts = [i for i, c in enumerate(k) if c == "S"]
    check(len(starts) == 2 and starts[0] == 0 and "?" not in k, "two start sequences, each followed by repeats (%s)" % k)
    if len(starts) != 2:
        return
    a, b = fr[0], fr[starts[1]]
    check(a.code() is not None and b.code() is not None and a.code() != b.code(),
          "the second start sequence is the second key's code (%s, %s)"
          % ("%08X" % a.code() if a.code() is not None else None, "%08X" % b.code() if b.code() is not None else None))
    b_ms = (b.start - t0) / 1000.0
    check(500 <= b_ms <= 500 + PRONTO_REPEAT_MS + MARGIN_MS,
          "the second key starts when the first key's frame on air is out (%.0f ms, pressed at 500)" % b_ms)
    check(not overlaps(fr), "no stream overlaps the next", str(overlaps(fr)))
    check(max_start_gap_ms(fr) <= PRONTO_REPEAT_MS + MARGIN_MS, "back to back across the change (max %.1f ms)" % max_start_gap_ms(fr))
    last_end = (fr[-1].end - t0) / 1000.0
    check(last_end >= 1300 - MARGIN_MS,
          "the first key's late release does not stop the second (last frame ends at %.0f ms)" % last_end)


def test_taps_and_sequences(hub, tx, keys, ws):
    print("[*] 6. taps and command lists: one start sequence each")
    tx.mark()
    t0 = time.monotonic()
    st, r = hub.http_post("/api/ir-send", {"deviceId": TV, "command": "VolumeUp"}, timeout=5)
    took = (time.monotonic() - t0) * 1000.0
    fr = tx.wait_quiet()
    check(st == 200 and kinds(fr) == "S", "a tap: one start sequence (%s)" % kinds(fr), "%s %s" % (st, r))
    check(fr and took >= fr[0].dur / 1000.0 - 5, "the tap returns when the frame is out (%.0f ms)" % took)

    tx.mark()
    ws.send_json({"action": "send_command", "deviceId": TV, "commands": ["VolumeUp", "VolumeDown"], "delay_secs": 0.2, "msgId": 61})
    ack, _ = ws.wait_for("ack", 5, lambda m: m.get("msgId") == 61)
    fr = tx.wait_quiet()
    codes = [f.code() for f in fr]
    check(ack is not None and ack.get("ok") is True and kinds(fr) == "SS" and codes[0] != codes[1],
          "a list of two commands: two start sequences (%s)" % kinds(fr), str(ack))

    tx.mark()
    st, r = hub.http_post("/api/activity", {"activity": "10"})
    fr = tx.wait_quiet(quiet=1.0, timeout=15)
    check(st == 200 and kinds(fr) == "SS", "an activity start (PowerOn, InputHdmi1): one start sequence per step (%s)" % kinds(fr),
          "%s %s" % (st, r))

    tx.mark()
    res = {}

    def tap():
        time.sleep(0.4)
        res["t"] = now_us()
        res["r"] = hub.http_post("/api/ir-send", {"deviceId": TV, "command": "VolumeDown"}, timeout=5)

    th = threading.Thread(target=tap)
    th.start()
    t0 = keys.play([(0, "press", "VolumeUp"), (300, "hold", "VolumeUp"), (600, "hold", "VolumeUp"),
                    (900, "hold", "VolumeUp"), (1200, "release", "VolumeUp")])
    th.join()
    fr = tx.wait_quiet()
    k = kinds(fr)
    starts = [i for i, c in enumerate(k) if c == "S"]
    check(len(starts) == 2 and starts[1] == len(fr) - 1 and set(k[1:-1]) <= {"r"},
          "a tap while a key is held: the key ends, the tap's start sequence is the last frame (%s)" % k)
    if len(starts) == 2:
        tap_ms = (fr[-1].start - res["t"]) / 1000.0
        check(0 <= tap_ms <= PRONTO_REPEAT_MS + MARGIN_MS and not overlaps(fr),
              "the tap waits only for the frame on air (starts %.0f ms after the request)" % tap_ms)


def test_hold_secs(hub, tx, ws):
    print("[*] 7. hold_secs of send_command")
    tx.mark()
    ws.drain(0.2)
    t0 = now_us()
    ws.send_json({"action": "send_command", "deviceId": TV, "command": "VolumeUp", "hold_secs": 1.2, "msgId": 71})
    ack, _ = ws.wait_for("ack", 5, lambda m: m.get("msgId") == 71)
    fr = tx.wait_quiet()
    k = kinds(fr)
    check(ack is not None and ack.get("ok") is True and len(fr) >= 9 and k[:1] == "S" and set(k[1:]) == {"r"},
          "the command repeats while it is held (%s)" % k, str(ack))
    if fr:
        last_end = (fr[-1].end - t0) / 1000.0
        check(1200 - MARGIN_MS <= last_end <= 1200 + PRONTO_REPEAT_MS + 2 * MARGIN_MS and max_start_gap_ms(fr) <= PRONTO_REPEAT_MS + MARGIN_MS,
              "for 1.2 s, back to back (last frame ends at %.0f ms)" % last_end)


def test_repeat_formats(hub, tx, keys):
    print("[*] 8. a code without a repeat sequence, an NEC type code, a Sony code")
    tx.mark()
    keys.play([(0, "press", "Whole", "9"), (300, "hold", "Whole", "9"), (600, "hold", "Whole", "9"), (800, "release", "Whole", "9")])
    fr = tx.wait_quiet()
    check(len(fr) >= 6 and all(f.code() == WHOLE_CODE for f in fr),
          "no repeat sequence: every frame is the whole code (%d frames, %s)"
          % (len(fr), sorted(set("%08X" % f.code() if f.code() is not None else "?" for f in fr))))
    per = [(b.start - a.start) / 1000.0 for a, b in zip(fr, fr[1:])]
    check(per and WHOLE_MS - 1.5 <= median(per) <= WHOLE_MS + 10 and abs(fr[0].dur / 1000.0 - WHOLE_MS) <= 1.5,
          "repeated at the frame's own length %.1f ms: stream %.1f ms, median start-to-start %.1f ms"
          % (WHOLE_MS, fr[0].dur / 1000.0 if fr else 0, median(per)))

    tx.mark()
    keys.play([(0, "press", "Nec", "9"), (300, "hold", "Nec", "9"), (600, "hold", "Nec", "9"), (800, "release", "Nec", "9")])
    fr = tx.wait_quiet()
    k = kinds(fr)
    check(len(fr) >= 6 and k[0] == "S" and set(k[1:]) == {"r"} and fr[0].code() == 0x20DF40BF,
          "NEC type: the frame once, then the repeat code (%s)" % k)
    rep = [(b.start - a.start) / 1000.0 for a, b in zip(fr[1:], fr[2:])]
    check(rep and NEC_REPEAT_MS - 1.5 <= median(rep) <= NEC_REPEAT_MS + 10,
          "the repeat code every %.1f ms (median %.1f ms)" % (NEC_REPEAT_MS, median(rep)))

    # Sony: single 45 ms frames (13 marks each), at least three for a press
    tx.mark()
    keys.play([(0, "press", "Sony", "9"), (300, "hold", "Sony", "9"), (500, "release", "Sony", "9")])
    fr = tx.wait_quiet()
    marks = [len(f.marks) for f in fr]
    check(len(fr) >= 6 and set(marks) == {13},
          "Sony: single frames while held (marks per stream %s)" % marks)
    rep = [(b.start - a.start) / 1000.0 for a, b in zip(fr[1:], fr[2:])]
    check(rep and 45 - 1.5 <= median(rep) <= 45 + 10, "one frame every 45 ms (median %.1f ms)" % median(rep))


def test_capture_mode(hub, tx, keys):
    print("[*] 9. capture mode on while a key is held")
    rx = os.path.join(hub.mock_dir, "i2s_rx")
    with open(rx, "wb") as f:
        f.write(b"\xff" * 4096)   # the line is idle
    ws = hub.ws()
    ws.recv_json(5)
    st, r = hub.http_post("/api/ir-capture", {"enabled": True})
    check(st == 200 and r.get("ir_capture") is True, "capture mode on", "%s %s" % (st, r))
    ws.drain(0.5)
    tx.mark()
    nec = nec_timings(0x20DF10EF)
    light = ir_rx_bitstream(nec, idle_before_bytes=1024, idle_after_us=200000)

    def own_light():
        # what the receiver sees of the Hub's own blaster: light between two frames of the hold
        for at in (0.23, 0.53, 0.83):
            time.sleep(max(0.0, t_start + at - time.monotonic()))
            with open(rx, "ab") as f:
                f.write(light)

    t_start = time.monotonic()
    th = threading.Thread(target=own_light)
    th.start()
    keys.play([(0, "press", "VolumeUp"), (300, "hold", "VolumeUp"), (600, "hold", "VolumeUp"),
               (900, "hold", "VolumeUp"), (1100, "release", "VolumeUp")])
    th.join()
    fr = tx.wait_quiet()
    leaked = [m for m in ws.drain(0.6) if m.get("type") == "ir_received"]
    check(len(fr) >= 9, "the key was held (%d frames)" % len(fr))
    check(leaked == [], "no ir_received event from the Hub's own held key", "%d event(s)" % len(leaked))
    time.sleep(0.2)   # the capture also drops 100 ms after a send
    with open(rx, "ab") as f:
        f.write(light)
    msg, frames = ws.wait_for("ir_received", 5)
    check(msg is not None, "after the release the next press is received", str(frames)[:200])
    hub.http_post("/api/ir-capture", {"enabled": False})
    ws.close()


def main():
    with tempfile.TemporaryDirectory(prefix="irhold_", dir="/tmp" if os.path.isdir("/tmp") else None) as tmp:
        sock_path = os.path.join(tmp, "keys.sock")
        log_path = os.path.join(tmp, "daemon.log")
        with HubHarness(port=PORT, log_path=log_path, args=["--key-socket", sock_path],
                        extra_config_files={"devices.json": devices()}) as hub:
            with open(os.path.join(hub.mock_dir, "i2s_tx_realtime"), "w") as f:
                f.write("1")
            st, _ = hub.http_post("/api/settings", {"dry_run": False})
            check(st == 200 and os.path.exists(sock_path), "daemon up: real sends through the mock transmitter, key channel bound")
            tx = Tx(hub)
            keys = Keys(sock_path)
            ws = hub.ws()
            ws.recv_json(5)
            test_held_key(hub, tx, keys, ws)
            time.sleep(0.3)
            test_press_does_not_block(hub, tx, keys, ws)
            time.sleep(0.3)
            test_lost_release(hub, tx, keys, log_path)
            time.sleep(0.3)
            test_old_remote_timing(hub, tx, keys)
            time.sleep(0.3)
            test_second_key(hub, tx, keys)
            time.sleep(0.3)
            test_taps_and_sequences(hub, tx, keys, ws)
            time.sleep(0.3)
            test_hold_secs(hub, tx, ws)
            time.sleep(0.3)
            test_repeat_formats(hub, tx, keys)
            time.sleep(0.3)
            test_capture_mode(hub, tx, keys)
            ws.close()
    if failed:
        print("[-] test_ir_hold: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_ir_hold passed")


if __name__ == "__main__":
    main()
