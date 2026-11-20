#!/usr/bin/env python3
"""USB Ethernet driver on demand: GET /api/network-status eth_driver and
POST /api/ethernet-driver-install.

- no state file: no eth_driver object;
- a state written by usb_eth_driver.sh scan (driver asix, usbnet and asix missing) shows up as
  eth_driver {vid, pid, driver, missing[], loaded, unsupported, installing};
- the install route takes only the driver the adapter needs: another name, a name with shell
  characters or no adapter answers 400; the right one answers 202 and marks the download running
  (the test build does not run the script); a second request while it runs answers 409;
- a finished download with a non-zero exit code shows install_failed.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_hub_daemon_test  # noqa: E402

PORT = 8937
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("[+] PASS: %s" % name)
    else:
        failed += 1
        print("[-] FAIL: %s %s" % (name, detail))


def main():
    build_hub_daemon_test()
    with tempfile.TemporaryDirectory() as tmp, HubHarness(port=PORT, env={"HARMONY_TEST_TMP_DIR": tmp}) as hub:
        st, r = hub.http_get("/api/network-status")
        check(st == 200 and "eth_driver" not in r, "no adapter: no eth_driver", str(r.get("eth_driver")))
        st, r = hub.http_post("/api/ethernet-driver-install", {"module": "asix"})
        check(st == 400, "no adapter: install refused", "%s %s" % (st, r))

        with open(os.path.join(tmp, "eth_driver_state"), "w") as f:
            f.write("vid=0b95\npid=7720\ndriver=asix\nmissing=usbnet asix\nloaded=0\nunsupported=0\n")
        st, r = hub.http_get("/api/network-status")
        d = r.get("eth_driver") or {}
        check(d.get("vid") == "0b95" and d.get("pid") == "7720" and d.get("driver") == "asix",
              "adapter and driver reported", str(d))
        check(d.get("missing") == ["usbnet", "asix"], "missing modules listed", str(d))
        check(d.get("loaded") is False and d.get("unsupported") is False and d.get("installing") is False,
              "flags reported", str(d))

        for bad in ("r8152", "asix;reboot", "", "../asix"):
            st, r = hub.http_post("/api/ethernet-driver-install", {"module": bad})
            check(st == 400, "refused module %r" % bad, "%s %s" % (st, r))
        check(not os.path.exists(os.path.join(tmp, "eth_driver_running")), "refused: nothing started")

        st, r = hub.http_post("/api/ethernet-driver-install", {"module": "asix"})
        check(st == 202 and r.get("status") == "installing", "needed driver accepted", "%s %s" % (st, r))
        st, r = hub.http_get("/api/network-status")
        check((r.get("eth_driver") or {}).get("installing") is True, "download shown as running", str(r.get("eth_driver")))
        st, r = hub.http_post("/api/ethernet-driver-install", {"module": "asix"})
        check(st == 409, "second download refused while one runs", "%s %s" % (st, r))

        os.unlink(os.path.join(tmp, "eth_driver_running"))
        with open(os.path.join(tmp, "eth_driver.rc"), "w") as f:
            f.write("1\n")
        st, r = hub.http_get("/api/network-status")
        check((r.get("eth_driver") or {}).get("install_failed") is True, "failed download reported", str(r.get("eth_driver")))
    if failed:
        print("eth driver API: %d FAILED" % failed)
        sys.exit(1)
    print("all eth driver API checks passed")


if __name__ == "__main__":
    main()
