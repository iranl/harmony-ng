#!/usr/bin/env python3
"""
Settings saved over HTTP while the main loop runs the MQTT client and the mDNS and SSDP
responders: ThreadSanitizer must find no data race in mqtt_client.c, hub_mqtt.c, hub_mdns.c or
hub_ssdp.c.

The HTTP worker that saved the settings used to disconnect and set up the MQTT client, and close,
reopen and rebuild the mDNS and SSDP responders, while the main loop read the same sockets and
records. The settings now wait for the main loop, and the MQTT client keeps its connection state
under one lock (the connect worker installs its socket only if nothing replaced it meanwhile).

Builds hub_daemon_test with -fsanitize=thread (run with ASLR off: setarch -R), a mock broker on
9105, daemon 9108, mDNS 9106, SSDP 9107, shim 9109. Skipped when the compiler has no TSan.
"""

import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, BUILD_DIR, SAMPLE_CONFIG, build_hub_daemon_test  # noqa: E402
from mock_mqtt import MockBroker  # noqa: E402

PORT = 9108
BROKER_PORT = 9105
MDNS_PORT = 9106
SSDP_PORT = 9107
LEGACY_PORT = 9109
WATCHED = ("mqtt_client.c", "hub_mqtt.c", "hub_mdns.c", "hub_ssdp.c")


def main():
    if not shutil.which("setarch"):
        print("[*] SKIP: setarch not found (TSan needs ASLR off)")
        return
    out = os.path.join(BUILD_DIR, "hub_daemon_test_tsan")
    try:
        build_hub_daemon_test(extra_cflags=["-fsanitize=thread", "-O1"], out=out)
    except RuntimeError:
        print("[*] SKIP: no ThreadSanitizer build here")
        return
    logdir = tempfile.mkdtemp(prefix="tsan_settings_")
    wrapper = os.path.join(logdir, "run.sh")
    with open(wrapper, "w") as f:
        f.write('#!/bin/sh\nexec setarch "$(uname -m)" -R "%s" "$@"\n' % out)
    os.chmod(wrapper, 0o755)
    with open(os.path.join(SAMPLE_CONFIG, "settings.json")) as f:
        settings = json.load(f)
    settings["legacy_enabled"] = True
    settings["mdns_enabled"] = True
    broker = MockBroker(port=BROKER_PORT)
    mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": BROKER_PORT, "baseTopic": "harmony/hub",
                "clientId": "harmony-tsan", "haDiscovery": False}
    env = {"HARMONY_MQTT_LIVE": "1",
           "TSAN_OPTIONS": "log_path=%s/tsan report_signal_unsafe=0 history_size=4" % logdir}
    args = ["--mdns-port", str(MDNS_PORT), "--ssdp-port", str(SSDP_PORT), "--legacy-port", str(LEGACY_PORT)]
    print("[-] settings saved while the main loop runs MQTT, mDNS and SSDP (ThreadSanitizer)")
    try:
        with HubHarness(port=PORT, hub_bin=wrapper, env=env, args=args,
                        extra_config_files={"settings.json": settings},
                        extra_data_files={"mqtt.json": mqtt_cfg}) as hub:
            time.sleep(1.0)
            for i in range(12):
                hub.http_post("/api/mqtt-save", {"baseTopic": "harmony/hub%d" % (i % 2), "haDiscovery": False})
                hub.http_post("/api/settings", {"hub_name": "Hub %d" % i, "mdns_enabled": i % 3 != 0,
                                                "legacy_enabled": i % 2 == 0})
                time.sleep(0.15)
            time.sleep(1.0)
            online = [t for t, v in broker.retained.items() if t.endswith("/status") and v == b"online"]
    finally:
        broker.close()
    if not online:
        print("  FAIL the Hub never connected to the mock broker")
        sys.exit(1)
    reports = []
    for path in glob.glob(os.path.join(logdir, "tsan*")):
        with open(path, errors="replace") as f:
            text = f.read()
        for block in text.split("==================")[1:]:
            # cJSON keeps the position of its last parse error in one global for every thread
            # (global_error): not a race of the code under test
            if "data race" in block and any(w in block for w in WATCHED) and "'global_error'" not in block:
                reports.append([ln for ln in block.strip().splitlines()
                                if "#0 " in ln or "#1 " in ln or "Location" in ln or "WARNING" in ln][:8])
    if os.environ.get("KEEP_TSAN_LOGS"):
        print("[*] TSan logs kept in %s" % logdir)
    else:
        shutil.rmtree(logdir, ignore_errors=True)
    if reports:
        print("  FAIL %d data race report(s) in %s" % (len(reports), ", ".join(WATCHED)))
        for r in reports[:6]:
            print("    " + "\n    ".join(r))
        sys.exit(1)
    print("  PASS no data race in %s" % ", ".join(WATCHED))
    print("[+] Settings across threads: all checks passed")


if __name__ == "__main__":
    main()
