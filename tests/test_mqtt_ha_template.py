#!/usr/bin/env python3
"""
The Home Assistant activity select's value_template (MQTT discovery) is a Jinja map from activity
id to name. With many activities with long names it was cut at 2047 bytes, in the middle of an
entry and without its closing part, so Home Assistant could not render it. With 32 activities of
63-byte names and 8-digit ids (the most the daemon loads) every entry and the closing part must be
there. hub_daemon_test publishes to the stdlib mock broker (HARMONY_MQTT_LIVE=1).
"""

import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, SAMPLE_CONFIG  # noqa: E402
from mock_mqtt import MockBroker  # noqa: E402

PORT = 8973
TOPIC = "homeassistant/select/harmony-test_activity/config"
CLOSING = "} %}{{ m.get(value_json.activity | string, 'PowerOff') }}"


def config_dir():
    d = tempfile.mkdtemp(prefix="ha_tpl_")
    acts = {"activities": []}
    for i in range(32):
        aid = str(70000000 + i)
        name = ("Activity %02d " % i).ljust(63, "x")
        acts["activities"].append({"id": aid, "name": name, "type": "Custom", "order": i + 1,
                                   "startSequence": [], "stopSequence": []})
    json.dump(acts, open(os.path.join(d, "activities.json"), "w"))
    for name in ("devices.json", "settings.json"):
        shutil.copy(os.path.join(SAMPLE_CONFIG, name), os.path.join(d, name))
    return d


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + detail))
        if not cond:
            failures.append(name)

    broker = MockBroker()
    mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": broker.port, "baseTopic": "harmony/hub",
                "clientId": "harmony-test", "discoveryPrefix": "homeassistant", "haDiscovery": True}
    cfg = config_dir()
    try:
        with HubHarness(port=PORT, config_dir=cfg, env={"HARMONY_MQTT_LIVE": "1"},
                        extra_data_files={"mqtt.json": mqtt_cfg}):
            deadline = time.time() + 10
            while time.time() < deadline and not broker.retained.get(TOPIC):
                time.sleep(0.1)
            raw = broker.retained.get(TOPIC)
            check(raw is not None, "activity select discovery config published")
            sel = json.loads(raw.decode("utf-8")) if raw else {}
            tpl = sel.get("value_template", "")
            check(tpl.startswith("{% set m = {'-1': 'PowerOff'"), "template starts with the map", tpl[:60])
            check(tpl.endswith(CLOSING), "template ends with the closing part (valid Jinja)", "len %d, ends %r" % (len(tpl), tpl[-60:]))
            ids = [str(70000000 + i) for i in range(32)]
            missing = [i for i in ids if ("'%s': '" % i) not in tpl]
            check(not missing, "all 32 activities are in the map", "missing %s" % missing)
            check(len(sel.get("options", [])) == 33, "options list PowerOff and the 32 activities", str(len(sel.get("options", []))))
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        try:
            broker.close()
        except Exception:
            pass

    if failures:
        print("ha activity template: %d FAILED" % len(failures))
        return 1
    print("all ha activity template checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
