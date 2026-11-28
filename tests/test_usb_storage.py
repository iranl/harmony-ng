#!/usr/bin/env python3
"""Data and cache on a USB stick (Linux 6.18 system).

build/kernel6/usb_storage.sh in a host sandbox (its /cache, /proc and /sys paths moved into a
temporary root, df stubbed):
- status: flash without the flag; usb with the flag and the stick on /usb (device and space);
  usb-missing with the flag and no stick;
- devices: the sd* disks of /sys/block with their size in KB;
- setup refuses without --yes, a name that is not a USB disk, and a Hub that is not on a root
  file system boot; nothing is written then;
- disable removes the flag and the missing marker.

GET/POST /api/usb-storage on the host test daemon (HARMONY_USB_STORAGE_SH: a fake script):
- the mode, the stick and its space, and the disks are reported;
- only scan, setup and disable are accepted; setup needs a disk the Hub lists and confirm: true
  (400 otherwise); an accepted action starts a background job (202), a second one while it runs
  answers 409; a failed job and its last output line are reported.
"""

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_hub_daemon_test  # noqa: E402

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "build" / "kernel6" / "usb_storage.sh"
PORT = 8936


class UsbStorageScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        r = self.root = pathlib.Path(self.tmp.name)
        for d in ("cache/kernel6", "proc", "sys/block", "stub"):
            (r / d).mkdir(parents=True)
        (r / "proc" / "mounts").write_text("rootfs / rootfs rw 0 0\n")
        df = r / "stub" / "df"
        df.write_text('#!/bin/sh\necho "Filesystem 1K-blocks Used Available"\necho "/dev/sda1 7800000 1000 7799000"\n')
        df.chmod(0o755)
        text = SCRIPT.read_text()
        for path in ("/cache/", "/proc/", "/sys/block/"):
            text = text.replace(path, f"{r}{path}")
        self.script = r / "usb_storage.sh"
        self.script.write_text(text)

    def tearDown(self):
        self.tmp.cleanup()

    def run_it(self, *args):
        env = dict(os.environ, PATH=f"{self.root}/stub:/usr/bin:/bin")
        return subprocess.run(["sh", str(self.script), *args], env=env, capture_output=True, text=True, timeout=30)

    def status(self):
        return dict(line.split("=", 1) for line in self.run_it("status").stdout.split())

    def test_status(self):
        self.assertEqual(self.status(), {"mode": "flash"})
        (self.root / "cache/kernel6/usb-storage").write_text("1234-abcd\n")
        self.assertEqual(self.status(), {"mode": "usb-missing", "uuid": "1234-abcd"})
        (self.root / "proc/mounts").write_text("/dev/sda1 /usb ext4 rw,noatime 0 0\n")
        st = self.status()
        self.assertEqual((st["mode"], st["device"], st["total_kb"], st["free_kb"]),
                         ("usb", "/dev/sda1", "7800000", "7799000"))

    def test_devices(self):
        for name, sectors in (("sda", 15600000), ("sdb", 2048)):
            (self.root / "sys/block" / name).mkdir()
            (self.root / "sys/block" / name / "size").write_text(f"{sectors}\n")
        (self.root / "sys/block/mtdblock3").mkdir()
        self.assertEqual(self.run_it("devices").stdout.split("\n")[:2], ["/dev/sda 7800000", "/dev/sdb 1024"])

    def test_setup_guards(self):
        for args, why in ((("setup", "/dev/sda1"), "--yes"), (("setup", "/dev/mtdblock3", "--yes"), "not a USB disk"),
                          (("setup", "/dev/sda1;reboot", "--yes"), "not a USB disk"),
                          # a disk no build machine has (a CI runner has a real /dev/sda1)
                          (("setup", "/dev/sdz9", "--yes"), "not found")):
            res = self.run_it(*args)
            self.assertNotEqual(res.returncode, 0, args)
            self.assertIn(why, res.stderr, args)
        self.assertFalse((self.root / "cache/kernel6/usb-storage").exists())

    def test_disable(self):
        self.assertNotEqual(self.run_it("disable").returncode, 0)
        (self.root / "cache/kernel6/usb-storage").write_text("1234-abcd\n")
        (self.root / "cache/kernel6/usb-missing").write_text("")
        self.assertEqual(self.run_it("disable").returncode, 0)
        self.assertEqual(list((self.root / "cache/kernel6").iterdir()), [])


FAKE = """#!/bin/sh
case "$1" in
  status) cat "$FAKE_DIR/status" 2>/dev/null || echo mode=flash ;;
  devices) cat "$FAKE_DIR/devices" 2>/dev/null ;;
esac
"""


class UsbStorageApi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_hub_daemon_test()

    def test_api(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = os.path.join(tmp, "usb_storage.sh")
            with open(fake, "w") as f:
                f.write(FAKE)
            env = {"HARMONY_TEST_TMP_DIR": tmp, "HARMONY_USB_STORAGE_SH": fake, "FAKE_DIR": tmp}
            with HubHarness(port=PORT, env=env) as hub:
                st, r = hub.http_get("/api/usb-storage")
                self.assertEqual(st, 200)
                self.assertEqual((r["mode"], r["disks"], r["busy"]), ("flash", [], False))

                with open(os.path.join(tmp, "devices"), "w") as f:
                    f.write("/dev/sda 7800000\n/dev/mtdblock3 4096\n")
                st, r = hub.http_get("/api/usb-storage")
                self.assertEqual(r["disks"], [{"device": "/dev/sda", "size_kb": 7800000}])

                for body in ({"action": "format"}, {"action": "eject;reboot"}, {},
                             {"action": "setup", "device": "/dev/sda"},
                             {"action": "setup", "device": "/dev/sdb", "confirm": True},
                             {"action": "setup", "device": "/dev/sda;reboot", "confirm": True}):
                    st, r = hub.http_post("/api/usb-storage", body)
                    self.assertEqual(st, 400, body)
                st, r = hub.http_post("/api/usb-storage", {"action": "setup", "device": "/dev/sda", "confirm": True})
                self.assertEqual((st, r.get("action")), (202, "setup"))
                st, r = hub.http_get("/api/usb-storage")
                self.assertTrue(r["busy"])
                st, r = hub.http_post("/api/usb-storage", {"action": "disable"})
                self.assertEqual(st, 409)

                os.unlink(os.path.join(tmp, "usb_storage_running"))
                with open(os.path.join(tmp, "usb_storage.rc"), "w") as f:
                    f.write("1\n")
                with open(os.path.join(tmp, "usb_storage.log"), "w") as f:
                    f.write("usb_storage: /dev/sda is too small\n\n")
                with open(os.path.join(tmp, "status"), "w") as f:
                    f.write("mode=usb\nuuid=1234\ndevice=/dev/sda1\ntotal_kb=7800000\nfree_kb=7799000\n")
                st, r = hub.http_get("/api/usb-storage")
                self.assertFalse(r["busy"])
                self.assertTrue(r.get("failed"))
                self.assertEqual(r["message"], "usb_storage: /dev/sda is too small")
                self.assertEqual((r["mode"], r["device"], r["total_kb"], r["free_kb"]),
                                 ("usb", "/dev/sda1", 7800000, 7799000))


if __name__ == "__main__":
    unittest.main(verbosity=2)
