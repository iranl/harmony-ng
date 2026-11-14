#!/usr/bin/env python3
"""
Static file cache against hub_daemon_test: a request path longer than the cache key, or one
spelled with "/./" or "//", is still served but not cached. Such paths used to be stored cut
(or under a new alias each time) and took a new slot with a copy of the file at every request,
until all slots held copies of one file; the cut key then answered with that file.
"""

import http.client
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8963
SIZE = 300 * 1024
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def get(hub, path):
    """(status, body bytes); the harness helper would decode the body as text"""
    try:
        conn = http.client.HTTPConnection(hub.host, hub.port, timeout=5)
        conn.request("GET", path)
        r = conn.getresponse()
        body = r.read()
        conn.close()
        return r.status, body
    except Exception as e:
        return 0, str(e).encode()


def rss_kib(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return 0


def main():
    print("[-] Static file cache keys")
    content = bytes((i * 7) & 0xFF for i in range(SIZE))
    with HubHarness(port=PORT) as hub:
        assets = os.path.join(hub.data_dir, "www", "assets")
        os.makedirs(assets, exist_ok=True)
        with open(os.path.join(assets, "big.js"), "wb") as f:
            f.write(content)

        for _ in range(3):
            st, body = get(hub, "/assets/big.js")
        check(st == 200 and body == content, "plain path served", "%s %d" % (st, len(body)))
        start = rss_kib(hub.proc.pid)

        long_path = "/" + "./" * 92 + "assets/big.js"  # 198 bytes, longer than the key
        for _ in range(40):
            st, body = get(hub, long_path)
        check(st == 200 and body == content, "long path still served", "%s %d" % (st, len(body)))
        for n in range(1, 41):  # short aliases, each one a different key
            st, body = get(hub, "/" + "./" * n + "assets/big.js")
        st2, body2 = get(hub, "//assets//big.js")
        check(st == 200 and body == content and st2 == 200 and body2 == content, "aliases still served")
        grown = rss_kib(hub.proc.pid) - start
        check(grown < 2048, "no copies of the file kept for long paths and aliases (%d KiB more)" % grown,
              "%d KiB" % grown)

        cut_key = long_path[:127]  # the key the long path used to be stored under (a directory)
        st, body = get(hub, cut_key)
        check(body != content, "the cut key does not answer with the file", "%s %d" % (st, len(body)))

        st, body = get(hub, "/assets/big.js")
        check(st == 200 and body == content, "plain path still served after all that")

    if failed:
        print("[-] %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] All static cache key checks passed")


if __name__ == "__main__":
    main()
