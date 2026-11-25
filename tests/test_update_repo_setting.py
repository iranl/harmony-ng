#!/usr/bin/env python3
"""The update_repo setting (POST/GET /api/settings).

- a usable http(s):// URL is stored in settings.json and returned, with update_repo_default;
- a value with a shell character, a space or another scheme is refused with 400 and nothing
  changes;
- a trailing slash or /bin/mips is stripped before it is stored; a URL without a host is refused;
- an empty value clears it (back to the official source).
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_hub_daemon_test  # noqa: E402

PORT = 8939
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("[+] PASS: %s" % name)
    else:
        failed += 1
        print("[-] FAIL: %s %s" % (name, detail))


def stored(hub):
    with open(os.path.join(hub.data_dir, "config", "settings.json")) as f:
        return json.load(f).get("update_repo")


def main():
    build_hub_daemon_test()
    fork = "https://raw.githubusercontent.com/someone/fork/ota"
    with HubHarness(port=PORT) as hub:
        st, r = hub.http_post("/api/settings", {"update_repo": fork})
        check(st == 200 and r.get("update_repo") == fork, "valid source saved", "%s %s" % (st, r))
        check(stored(hub) == fork, "valid source in settings.json")
        st, r = hub.http_get("/api/settings")
        check(r.get("update_repo") == fork, "GET returns the source", str(r))
        check(r.get("update_repo_default", "").startswith("https://raw.githubusercontent.com/"),
              "GET returns the default", str(r))

        for bad in ("https://x.invalid/a';reboot;'", "https://x.invalid/a b", "ftp://x.invalid/ota",
                    "file:///etc/passwd"):
            st, r = hub.http_post("/api/settings", {"update_repo": bad})
            check(st == 400, "refused: %s" % bad, "%s %s" % (st, r))
        check(stored(hub) == fork, "refused values change nothing")

        # stored in the form every consumer reads: no trailing slash, no old /bin/mips suffix
        for typed in (fork + "/", fork + "/bin/mips", fork + "/bin/mips/"):
            st, r = hub.http_post("/api/settings", {"update_repo": typed})
            check(st == 200 and stored(hub) == fork, "normalised: %s" % typed, "%s %s" % (st, r))
        for bad in ("https://", "https:///ota", "http://bin/mips"):
            st, r = hub.http_post("/api/settings", {"update_repo": bad})
            check(st == 400, "refused (no host): %s" % bad, "%s %s" % (st, r))
        check(stored(hub) == fork, "values without a host change nothing")

        st, r = hub.http_post("/api/settings", {"update_repo": ""})
        check(st == 200 and stored(hub) == "", "empty clears the source", "%s %s" % (st, r))
    if failed:
        print("update_repo setting: %d FAILED" % failed)
        sys.exit(1)
    print("all update_repo setting checks passed")


if __name__ == "__main__":
    main()
