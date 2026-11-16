#!/usr/bin/env python3
"""
What the MQTT send_command sequencer really sends (hub_mqtt.c seq_tick / hub_mqtt_queue_commands),
not only what it answers. hub_daemon_test runs in dry-run mode with MQTT live against the stdlib
mock broker; a thread follows the daemon log and stamps each line with its arrival time:

  "[*] MQTT seq [i/n] ... cmd='<name>'"   one per queued step, before the step is sent
  "[DRY-RUN] IR ... simulated"            one per IR frame really handed to the sender

 1  a two-command job: both steps, in order, the second at least delay_secs after the first,
    each followed by a send
 2  num_repeats 2: four steps Up, Down, Up, Down, each sent
 3  a single command that arrives while a job runs waits behind it (sent after the job's last
    step, through the queue), not in the middle of the job
 4  hold_secs: PRESS step, then RELEASE step at least hold_secs later

The command channel tests in test_mqtt_v2.py check the replies only; a sequencer that sent
nothing passed them.
"""

import json
import os
import re
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402
from mock_mqtt import MockBroker, MockClient  # noqa: E402

PORT = 9267
BASE = "harmony/seqtest"
failed = 0

SEQ_RE = re.compile(r"MQTT seq \[(\d+)/(\d+)\] \(repeat (\d+)/(\d+)\).*cmd='([^']*)'( \[(PRESS|RELEASE)\])?")


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


class LogFollower(threading.Thread):
    """Reads the daemon log as it grows and keeps (arrival time, line) pairs."""

    def __init__(self, path):
        threading.Thread.__init__(self, daemon=True)
        self.path = path
        self.lines = []
        self.lock = threading.Lock()
        self.stop_flag = False

    def run(self):
        pos, part = 0, b""
        while not self.stop_flag:
            try:
                with open(self.path, "rb") as f:
                    f.seek(pos)
                    data = f.read()
                    pos = f.tell()
            except OSError:
                data = b""
            if data:
                now = time.monotonic()
                part += data
                *full, part = part.split(b"\n")
                with self.lock:
                    for ln in full:
                        self.lines.append((now, ln.decode("utf-8", "replace")))
            time.sleep(0.01)

    def mark(self):
        with self.lock:
            return len(self.lines)

    def since(self, mark):
        with self.lock:
            return list(self.lines[mark:])


def events(lines):
    """[(t, 'seq', cmd, tag) | (t, 'send', None, None)] in log order"""
    out = []
    for t, ln in lines:
        m = SEQ_RE.search(ln)
        if m:
            out.append((t, "seq", m.group(5), m.group(7)))
        elif "[DRY-RUN] IR" in ln and "simulated" in ln:
            out.append((t, "send", None, None))
    return out


def wait_events(log, mark, pred, timeout=6.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ev = events(log.since(mark))
        if pred(ev):
            return ev
        time.sleep(0.05)
    return events(log.since(mark))


def steps_sent(ev):
    """[(t, cmd, sent)] for each seq step: sent = a send line before the next seq line"""
    out = []
    for i, e in enumerate(ev):
        if e[1] != "seq":
            continue
        sent = False
        for f in ev[i + 1:]:
            if f[1] == "seq":
                break
            if f[1] == "send":
                sent = True
                break
        out.append((e[0], e[2], sent, e[3]))
    return out


def main():
    broker = MockBroker()
    mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": broker.port, "baseTopic": BASE,
                "clientId": "harmony-seqtest", "haDiscovery": False}
    log_path = tempfile.mktemp(prefix="mqtt_seq_", suffix=".log")
    try:
        with HubHarness(port=PORT, env={"HARMONY_MQTT_LIVE": "1"}, log_path=log_path,
                        extra_data_files={"mqtt.json": mqtt_cfg}):
            log = LogFollower(log_path)
            log.start()
            cmd = MockClient("127.0.0.1", broker.port, "seq-commander")
            cmd.subscribe(BASE + "/cmd/response")
            deadline = time.time() + 10
            while time.time() < deadline:
                cmd.publish(BASE + "/cmd", json.dumps({"id": "p0", "action": "ping"}), qos=1)
                if cmd.wait_for(BASE + "/cmd/response", 0.5):
                    break
            check(time.time() < deadline, "daemon connected to the broker")

            def send(msg):
                cmd.publish(BASE + "/cmd", json.dumps(msg), qos=1)

            print("[-] 1  two commands with delay_secs 0.3")
            mark = log.mark()
            send({"id": "s1", "action": "send_command", "deviceId": "1",
                  "command": ["VolumeUp", "VolumeDown"], "delay_secs": 0.3})
            st = steps_sent(wait_events(log, mark, lambda ev: len(steps_sent(ev)) >= 2 and ev[-1][1] == "send"))
            check([s[1] for s in st] == ["VolumeUp", "VolumeDown"], "steps in order", str(st))
            check(len(st) == 2 and all(s[2] for s in st), "every step sent", str(st))
            if len(st) == 2:
                gap = st[1][0] - st[0][0]
                check(gap >= 0.27, "second step at least delay_secs after the first", "%.3f s" % gap)

            print("[-] 2  num_repeats 2")
            time.sleep(0.5)
            mark = log.mark()
            send({"id": "s2", "action": "send_command", "deviceId": "1",
                  "command": ["VolumeUp", "VolumeDown"], "num_repeats": 2})
            st = steps_sent(wait_events(log, mark, lambda ev: len(steps_sent(ev)) >= 4 and ev[-1][1] == "send"))
            check([s[1] for s in st] == ["VolumeUp", "VolumeDown", "VolumeUp", "VolumeDown"],
                  "four steps, repeated in order", str([s[1] for s in st]))
            check(len(st) == 4 and all(s[2] for s in st), "every step sent", str(st))

            print("[-] 3  a single command waits behind a running job")
            time.sleep(0.6)
            mark = log.mark()
            send({"id": "s3a", "action": "send_command", "deviceId": "1",
                  "command": ["VolumeUp", "VolumeDown"], "delay_secs": 0.5})
            time.sleep(0.1)
            send({"id": "s3b", "action": "send_command", "deviceId": "1", "command": "PowerOff"})
            ev = wait_events(log, mark, lambda ev: len([e for e in ev if e[1] == "send"]) >= 3, timeout=8)
            seq_names = [e[2] for e in ev if e[1] == "seq"]
            check(seq_names == ["VolumeUp", "VolumeDown", "PowerOff"],
                  "PowerOff is queued and runs after the job", str(seq_names))
            # nothing is sent between the job's two steps
            idx = [i for i, e in enumerate(ev) if e[1] == "seq"]
            between = [e for e in ev[idx[0] + 1:idx[1]] if e[1] == "send"] if len(idx) >= 2 else []
            check(len(between) == 1, "only VolumeUp is sent between the job's steps", str(len(between)))

            print("[-] 4  hold_secs 0.4")
            time.sleep(0.6)
            mark = log.mark()
            send({"id": "s4", "action": "send_command", "deviceId": "1", "command": "VolumeUp",
                  "hold_secs": 0.4})
            ev = wait_events(log, mark, lambda ev: any(e[1] == "seq" and e[3] == "RELEASE" for e in ev))
            hold = [(e[0], e[3]) for e in ev if e[1] == "seq"]
            check([h[1] for h in hold] == ["PRESS", "RELEASE"], "PRESS then RELEASE", str(hold))
            if len(hold) == 2:
                check(hold[1][0] - hold[0][0] >= 0.37, "RELEASE at least hold_secs after PRESS",
                      "%.3f s" % (hold[1][0] - hold[0][0]))
            log.stop_flag = True
            cmd.close()
    finally:
        broker.close()
        try:
            os.unlink(log_path)
        except OSError:
            pass

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] MQTT sequencer sends: all checks passed")


if __name__ == "__main__":
    main()
