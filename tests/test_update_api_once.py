#!/usr/bin/env python3
"""POST /api/update-apply and /api/update-check on the test daemon (private mock root, port 9145).

- a second update-apply while one runs gets 409 "busy" (the "updating" flag of the first run is
  present and recent); a flag older than 30 minutes, left by a run that never ended, does not
  block, nor does one dated in the future (clock set back). The test build points the flag at a temporary file (HARMONY_TEST_UPDATING_FLAG) and
  never runs the updater;
- GET /api/update-status reports update_state from the state file update.sh writes (running,
  restarting with update_target until the daemon runs that version, failed with the last "[-]" line of the apply log as update_error),
  and a stale flag is not a running update;
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
    env = {"HARMONY_TEST_UPDATING_FLAG": flag, "HARMONY_TEST_UPDATE_STATE": state, "HARMONY_TEST_UPDATE_LOG": log}

    def write(path, text):
        with open(path, "w") as f:
            f.write(text)

    try:
        with HubHarness(port=PORT, env=env) as hub:
            st, r = hub.http_get("/api/update-status")
            check(st == 200 and r.get("update_state") == "idle" and r.get("updating") is False,
                  "status before any update: idle", (st, r))
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and r.get("status") == "applying", "first apply starts", (st, r))
            check(os.path.exists(flag), "the updating flag is set")
            write(log, "[*] Starting software update from x...\n[*] Checking for unified Hub package\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "running" and r.get("updating") is True
                  and r.get("update_step") == "[*] Checking for unified Hub package",
                  "status while it runs: running, with the last log line", r)
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 409 and r.get("status") == "busy", "a second apply while one runs: 409", (st, r))
            old = time.time() - 3600
            open(flag, "a").close()
            os.utime(flag, (old, old))
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and r.get("status") == "applying", "a flag older than 30 minutes does not block", (st, r))
            future = time.time() + 3600
            os.utime(flag, (future, future))
            st, r = hub.http_post("/api/update-apply", {})
            check(st == 200 and r.get("status") == "applying",
                  "a flag dated in the future (clock set back) does not block", (st, r))
            if os.path.exists(flag):
                os.unlink(flag)

            # update.sh ended with an error: flag removed, state failed
            write(state, "state=failed\n")
            write(log, "[*] Starting\n[-] Update refused: no valid release signature from x. Nothing was changed.\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "failed" and r.get("updating") is False
                  and r.get("update_error", "").startswith("[-] Update refused"), "failed with the error line", r)
            # installed, services restarting: the flag is gone but the update is not over
            write(state, "state=restarting\nversion=99.0.0\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "restarting" and r.get("updating") is True
                  and r.get("update_target") == "99.0.0", "restarting with the target version", r)
            # the state file stays in /tmp: a daemon that runs the version installed is done
            write(state, "state=restarting\nversion=0.0.1\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "idle" and r.get("updating") is False and "update_target" not in r,
                  "restarted daemon with the version installed: idle", r)
            # an update.sh older than the state file leaves state=running behind at its exit
            write(state, "state=running\n")
            st, r = hub.http_get("/api/update-status")
            check(r.get("update_state") == "idle" and r.get("updating") is False,
                  "running state without the flag: idle", r)
            write(flag, "")
            old = time.time() - 3600
            os.utime(flag, (old, old))
            st, r = hub.http_get("/api/update-status")
            check(r.get("updating") is False, "a flag older than 30 minutes: not updating", r)
            for pth in (flag, state, log):
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
    finally:
        for pth in (flag, state, log):
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
