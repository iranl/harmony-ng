#!/usr/bin/env python3
"""
Icon proxy download limits (GET /api/custom-icons/proxy-fetch), with a stand-in for
"busybox wget" under the mock /cache (no network): a server that keeps sending is cut off at
2 MB during the download (413), a server that sends too slowly is cut off at the time limit
(3 s in this build), two fetches at once each get their own image, and no temporary file is
left in /tmp.
"""

import os
import socket
import stat
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_hub_daemon_test, BUILD_DIR  # noqa: E402

PORT = 8906
PROXY_BIN = os.path.join(BUILD_DIR, "hub_daemon_test_proxy")
failed = 0

FAKE_WGET = r"""#!/bin/sh
# stand-in for "busybox wget ... -O <file> <url>": the last part of the URL says what to send
out=""; url=""
while [ $# -gt 0 ]; do
    case "$1" in
        -O) out="$2"; shift ;;
        http*) url="$1" ;;
    esac
    shift
done
dir=$(dirname "$0")
case "$url" in
    */big)   # 8 MB in 256 KB pieces, about 5 MB/s
        n=0
        while [ $n -lt 32 ]; do
            head -c 262144 /dev/zero >> "$out"; n=$((n + 1)); echo $n > "$dir/big.count"; sleep 0.05
        done ;;
    */slow)  # one byte a second for 12 s
        n=0
        while [ $n -lt 12 ]; do printf x >> "$out"; n=$((n + 1)); sleep 1; done ;;
    */small-*)  # 1000 bytes of the given letter, then a second before wget ends
        tag=${url##*/small-}
        head -c 1000 /dev/zero | tr '\000' "$tag" > "$out"; sleep 1 ;;
    *) exit 1 ;;
esac
exit 0
"""


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def fetch(port, what, timeout=30):
    """(status, body bytes, seconds)"""
    t0 = time.time()
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        s.sendall(("GET /api/custom-icons/proxy-fetch?url=http%%3A%%2F%%2Ficons.invalid%%2F%s HTTP/1.1\r\n"
                   "Host: 127.0.0.1\r\nConnection: close\r\n\r\n" % what).encode())
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        s.close()
    except OSError:
        return 0, b"", time.time() - t0
    head, _, body = data.partition(b"\r\n\r\n")
    parts = head.split(b" ", 2)
    return (int(parts[1]) if len(parts) > 1 else 0), body, time.time() - t0


def proxy_tmp_files():
    return {n for n in os.listdir("/tmp") if n.startswith("proxy_icon")}


def main():
    build_hub_daemon_test(force=not os.path.exists(PROXY_BIN), extra_cflags=["-DPROXY_FETCH_MAX_MS=3000"],
                          out=PROXY_BIN)
    print("[-] Icon proxy download limits")
    before_tmp = proxy_tmp_files()
    with HubHarness(port=PORT, hub_bin=PROXY_BIN) as hub:
        bindir = os.path.join(hub.mock_dir, "cache", "bin")
        os.makedirs(bindir, exist_ok=True)
        fake = os.path.join(bindir, "busybox")
        with open(fake, "w") as f:
            f.write(FAKE_WGET)
        os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

        st, body, _ = fetch(PORT, "small-Q")
        check(st == 200 and body == b"Q" * 1000, "a small image is passed on", "status %d, %d bytes" % (st, len(body)))

        st, _, dt = fetch(PORT, "big")
        time.sleep(0.5)
        with open(os.path.join(bindir, "big.count")) as f:
            pieces = int(f.read().strip() or 0)
        check(st == 413, "a download past 2 MB: 413", "status %d" % st)
        check(pieces <= 12, "the download was stopped near 2 MB, not after it ended",
              "%d of 32 pieces of 256 KB written in %.1f s" % (pieces, dt))

        st, _, dt = fetch(PORT, "slow")
        check(st == 502 and dt < 6, "a download slower than the time limit is stopped",
              "status %d after %.1f s" % (st, dt))

        results = {}

        def one(tag):
            results[tag] = fetch(PORT, "small-" + tag)

        ta = threading.Thread(target=one, args=("A",))
        tb = threading.Thread(target=one, args=("B",))
        ta.start()
        time.sleep(0.3)
        tb.start()
        ta.join(20)
        tb.join(20)
        ok_a = results.get("A", (0, b""))[:2] == (200, b"A" * 1000)
        ok_b = results.get("B", (0, b""))[:2] == (200, b"B" * 1000)
        check(ok_a and ok_b, "two fetches at once each get their own image",
              "A %s, B %s" % ((results.get("A") or (0, b""))[0:1], (results.get("B") or (0, b""))[0:1]))
        time.sleep(0.5)
    left = proxy_tmp_files() - before_tmp
    check(not left, "no temporary file left in /tmp", str(left))

    if failed:
        print("[!] test_icon_proxy_limits: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_icon_proxy_limits: all checks passed")


if __name__ == "__main__":
    main()
