#!/usr/bin/env python3
"""POST /api/update-apply and /api/update-check on the test daemon (private mock root, port 9145).

- a second update-apply while one runs gets 409 "busy", and so does one while the services restart
  after an update; a run older than 30 minutes on the uptime clock (since= of the state file) does
  not block, and a clock step (SNTP) does not end a running one. Without since= (an older
  update.sh) the flag's date counts: older than 30 minutes or in the future does not block. The
  test build points the flag, state, log and uptime at temporary files (HARMONY_TEST_*) and never
  runs the updater;
- GET /api/update-status reports update_state from the state file update.sh writes (running,
  restarting with update_target until the daemon runs that version, failed with the first "[-]"
  line of the apply log as update_error; failed wins over a flag a killed run left behind), and
  /api/status "updating" agrees; a daemon start ends "restarting" and keeps "failed";
- update-check with remote_version writes latest_version and update_available.flag only when
  they change: repeating the same check leaves both files untouched (flash writes).
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_mock_lib  # noqa: E402

PORT = 9145


def main():
    failures = []

    def check(cond, name, detail=""):
        print("  %s %s%s" % ("PASS" if cond else "FAIL", name, "" if cond or not detail else ": " + str(detail)))
        if not cond:
            failures.append(name)

    build_mock_lib()
    flag_dir = tempfile.mkdtemp(prefix="hub_updating_")
    flag = os.path.join(flag_dir, "hub_updating.flag")
    state = os.path.join(flag_dir, "hub_update.state")
    log = os.path.join(flag_dir, "update_apply.log")
    uptime = os.path.join(flag_dir, "uptime")
    env = {"HARMONY_TEST_UPDATING_FLAG": flag, "HARMONY_TEST_UPDATE_STATE": state, "HARMONY_TEST_UPDATE_LOG": log,
           "HARMONY_TEST_UPTIME_FILE": uptime}

    def write(path, text):
        with open(path, "w") as f:
            f.write(text)

    def age(path, secs):
        t = time.time() + secs
        os.utime(path, (t, t))

    try:
        write(uptime, "1000.50 0.0\n")
        with HubHarness(port=PORT, env=env) as hub:
            st, r = hub.http_get("/api/update-status")
            check(st == 200 and r.get("update_state") == "idle" and r.get("updating") is False,
                  "status before any update: idle", (st, r))
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and r.get("status") == "applying", "first apply starts", (st, r))
            check(os.path.exists(flag), "the updating flag is set")
            with open(state) as f:
                check(f.read() == "state=running\nsince=1000\n", "the apply records running on the uptime clock")
            write(log, "[*] Starting software update from x...\n[*] Checking for unified Hub package\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "running" and r.get("updating") is True
                  and r.get("update_step") == "[*] Checking for unified Hub package",
                  "status while it runs: running, with the last log line", r)
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 409 and r.get("status") == "busy", "a second apply while one runs: 409", (st, r))
            st, r = hub.http_get("/api/status")
            check(r.get("updating") is True, "/api/status says updating too", r)
            st, r = hub.http_post("/api/system-action", {"action": "reboot"})
            check(st == 409, "no reboot while an update runs", (st, r))
            # SNTP set the clock after the flag was written: the flag looks an hour old
            age(flag, -3600)
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "running", "a clock step does not end a running update", r)
            # 30 minutes on the uptime clock: a run that never ended
            write(uptime, "2801.00 0.0\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "idle" and r.get("updating") is False,
                  "running for more than 30 minutes of uptime: idle", r)
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and r.get("status") == "applying", "a run that never ended does not block", (st, r))

            # an update.sh older than the state file: state=running without since, the flag's date
            write(state, "state=running\n")
            age(flag, -3600)
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and r.get("status") == "applying", "a flag older than 30 minutes does not block", (st, r))
            write(state, "state=running\n")
            age(flag, 3600)
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and r.get("status") == "applying",
                  "a flag dated in the future (clock set back) does not block", (st, r))
            os.unlink(flag)
            write(state, "state=running\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "idle" and r.get("updating") is False,
                  "running state without the flag: idle", r)

            # update.sh ended with an error: the first "[-]" line is the cause
            write(state, "state=failed\nsince=2801\n")
            write(log, "[*] Starting\n[-] Update refused: no valid release signature from x. Nothing was changed.\n"
                       "[-] The software update ended with exit code 1.\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "failed" and r.get("updating") is False
                  and r.get("update_error", "").startswith("[-] Update refused"), "failed with the first error line", r)
            # killed without its EXIT trap: a fresh flag stays, failed still wins
            write(flag, "")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "failed" and r.get("updating") is False, "failed wins over a left flag", r)
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and r.get("status") == "applying", "an apply after a failed one starts", (st, r))
            os.unlink(flag)

            # installed, services restarting: the flag is gone but the update is not over
            write(state, "state=restarting\nversion=99.0.0\nsince=2900\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "restarting" and r.get("updating") is True
                  and r.get("update_target") == "99.0.0", "restarting with the target version", r)
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 409 and r.get("status") == "busy", "no apply while the services restart", (st, r))
            st, r = hub.http_post("/api/system-action", {"action": "reboot"})
            check(st == 409, "no reboot while the services restart", (st, r))
            # no daemon started for 10 minutes of uptime: the restart is over
            write(uptime, "3501.00 0.0\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "idle" and r.get("updating") is False,
                  "restarting older than 10 minutes: idle", r)
            write(uptime, "2950.00 0.0\n")
            st, r = hub.http_get("/api/status")
            check(r.get("updating") is True, "/api/status: updating while restarting", r)
            # a daemon that runs the version installed is done
            write(state, "state=restarting\nversion=0.0.1\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "idle" and r.get("updating") is False and "update_target" not in r,
                  "restarted daemon with the version installed: idle", r)
            for pth in (flag, log):
                if os.path.exists(pth):
                    os.unlink(pth)

            latest = os.path.join(hub.data_dir, "latest_version")
            avail = os.path.join(hub.data_dir, "update_available.flag")
            st, r = hub.http_post("/api/update-check", {"remote_version": "v99.0.0"})
            check(st == 200 and r.get("update_available") is True, "check with a newer version", (st, r))
            before = (os.stat(latest).st_mtime_ns, os.stat(avail).st_mtime_ns)
            time.sleep(0.05)
            hub.http_post("/api/update-check", {"remote_version": "v99.0.0"})
            after = (os.stat(latest).st_mtime_ns, os.stat(avail).st_mtime_ns)
            check(after == before, "the same check again writes nothing", (before, after))
            st, r = hub.http_post("/api/update-check", {"remote_version": "v0.0.1"})
            check(st == 200 and r.get("update_available") is False and not os.path.exists(avail),
                  "an older version clears the flag", (st, r))

        # The daemon that starts is the restart: "restarting" ends even when the version installed
        # is not the one it runs (a deploy of an older build, a manifest above the binary)
        write(state, "state=restarting\nversion=99.0.0\n")
        with HubHarness(port=PORT, env=env) as hub:
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "idle" and r.get("updating") is False and not os.path.exists(state),
                  "a daemon start ends restarting", r)
        write(state, "state=failed\n")
        with HubHarness(port=PORT, env=env) as hub:
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "failed", "a daemon start keeps failed", r)
    finally:
        for pth in (flag, state, log, uptime):
            if os.path.exists(pth):
                os.unlink(pth)
        os.rmdir(flag_dir)

    if failures:
        print("update API: %d FAILED" % len(failures))
        return 1
    print("all update API checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
