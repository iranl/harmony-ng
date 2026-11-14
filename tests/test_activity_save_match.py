#!/usr/bin/env python3
"""
POST /api/activity-save against hub_daemon_test: the id selects the activity to change.
A save without an id creates a new activity even when its name equals an existing one
(it used to overwrite that activity's devices and sequences), and a save with an id never
changes another activity that has the posted name.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8962
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def activities(hub):
    with open(os.path.join(hub.data_dir, "config", "activities.json")) as f:
        return json.load(f).get("activities", [])


def by_id(acts, act_id):
    for a in acts:
        if str(a.get("id")) == act_id:
            return a
    return {}


def summary(act):
    """What a wrong match would change: name, type, icon, devices and the commands of both sequences
    (the daemon writes steps in canonical form, so compare the meaningful fields only)"""
    seq = lambda k: [(st.get("type"), st.get("command"), st.get("deviceId"), st.get("delay")) for st in act.get(k, [])]
    return json.dumps([act.get("name"), act.get("type"), act.get("icon"), act.get("deviceIds"),
                       seq("startSequence"), seq("stopSequence")])


def main():
    print("[-] POST /api/activity-save matches by id only")
    step = [{"type": "Command", "command": "Home", "deviceId": "2"}]
    with HubHarness(port=PORT) as hub:
        # One save by id first: the daemon rewrites the whole file with canonical steps
        hub.http_post("/api/activity-save", {"id": "11", "name": "Media Player"})
        before = activities(hub)
        watch = summary(by_id(before, "10"))
        media = by_id(before, "11")
        check(by_id(before, "10").get("name") == "Watch TV" and media, "sample config has activities 10 and 11")

        # A new activity (no id) named like an existing one, in another letter case
        st, r = hub.http_post("/api/activity-save", {"id": "", "name": "watch tv", "startSequence": step})
        after = activities(hub)
        check(st == 200, "save without an id answered 200", "%s %s" % (st, r))
        check(summary(by_id(after, "10")) == watch, "activity 10 left unchanged",
              summary(by_id(after, "10"))[:200])
        new = [a for a in after if a.get("name") == "watch tv"]
        check(len(after) == len(before) + 1 and len(new) == 1 and new[0].get("startSequence") == step,
              "a new activity was created", "%d -> %d" % (len(before), len(after)))

        # Two new activities with the web UI's default name: both are kept
        for _ in range(2):
            hub.http_post("/api/activity-save", {"name": "New Activity", "startSequence": step})
        after2 = activities(hub)
        check(len([a for a in after2 if a.get("name") == "New Activity"]) == 2, "two 'New Activity' saves give two activities",
              str([a.get("name") for a in after2]))

        # Rename 11 to the name of 10: 11 changes, 10 does not
        st, r = hub.http_post("/api/activity-save", {"id": "11", "name": "Watch TV", "startSequence": step})
        after3 = activities(hub)
        check(st == 200 and by_id(after3, "11").get("name") == "Watch TV" and by_id(after3, "11").get("startSequence") == step,
              "activity 11 renamed and saved", json.dumps(by_id(after3, "11"))[:200])
        check(summary(by_id(after3, "10")) == watch, "activity 10 untouched by the rename of 11",
              summary(by_id(after3, "10"))[:200])

        # Update by id still works
        st, r = hub.http_post("/api/activity-save", {"id": "10", "name": "Watch TV 2", "startSequence": step})
        check(st == 200 and by_id(activities(hub), "10").get("name") == "Watch TV 2", "save by id updates that activity")

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] All activity save matching checks passed")


if __name__ == "__main__":
    main()
