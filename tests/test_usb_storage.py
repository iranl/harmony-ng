#!/usr/bin/env python3
"""USB stick support for non-essential files.

payload/scripts/usb_storage.sh scan in a host sandbox (fake sysfs, insmod/mount stubs):
- a stick behind a USB hub (interface class 08) without the storage modules installed is
  reported present with the missing modules, and nothing is loaded or mounted;
- with the modules installed they are loaded in order (scsi_mod, sd_mod, usb-storage), the
  partition gets a /dev node and is mounted on /mnt/usb;
- no stick: nothing is reported present.

GET/POST /api/usb-storage on the host test daemon:
- the state file is reported (present, mounted, device, fs, missing, free space, offload);
- only the known actions are accepted (400 otherwise); one starts a background job (202) and a
  second while it runs answers 409; a failed job and its last output line are reported.
"""

import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness, build_hub_daemon_test  # noqa: E402

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "usb_storage.sh"
PORT = 8936


class UsbStorageScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_scan(self, stick: bool, modules: bool):
        root = self.root
        usb = root / "sys" / "bus" / "usb" / "devices"
        usb.mkdir(parents=True)
        hub = usb / "1-1"
        hub.mkdir()
        (hub / "bDeviceClass").write_text("09\n")
        if stick:
            dev = usb / "1-1.3"
            (dev / "1-1.3:1.0").mkdir(parents=True)
            (dev / "1-1.3:1.0" / "bInterfaceClass").write_text("08\n")
            part = root / "sys" / "block" / "sda" / "sda1"
            part.mkdir(parents=True)
            (part / "dev").write_text("8:1\n")
            (root / "sys" / "block" / "sda" / "dev").write_text("8:0\n")
        for d in ("tmp", "dev", "mnt", "cache/modules", "data/harmony/config", "data/harmony/custom_icons"):
            (root / d).mkdir(parents=True, exist_ok=True)
        if modules:
            for m in ("scsi_mod", "sd_mod", "usb-storage"):
                (root / "cache" / "modules" / f"{m}.ko").write_text("")
        log = root / "calls.log"
        stub = root / "stub"
        stub.mkdir()
        (stub / "busybox").write_text(
            "#!/bin/sh\n"
            f"echo \"$*\" >> '{log}'\n"
            'case "$1" in df) echo "Filesystem 1K-blocks Used Available"; echo "/dev/sda1 7800000 100 7799900";; esac\n'
            "exit 0\n")
        (stub / "insmod").write_text(f"#!/bin/sh\necho \"insmod $1\" >> '{log}'\n")
        (stub / "sleep").write_text("#!/bin/sh\nexit 0\n")
        for p in stub.iterdir():
            p.chmod(0o755)
        text = SCRIPT.read_text(encoding="utf-8").replace("BB=/cache/bin/busybox\n", "BB=@STUB@\n")
        text = text.replace('[ -x "$BB" ] || BB=busybox\n', "")
        text = re.sub(r"(?<![\w./$-])/(data|sys|cache|tmp|dev|mnt)(?=[/ \n])", lambda m: f"{root}/{m.group(1)}", text)
        text = text.replace("@STUB@", f"{stub}/busybox")
        script = root / "usb_storage.sh"
        script.write_text(text)
        env = dict(os.environ, PATH=f"{stub}:/usr/bin:/bin")
        subprocess.run(["sh", str(script), "scan"], env=env, capture_output=True, text=True, timeout=30)
        state = root / "tmp" / "usb_storage_state"
        st = dict(l.split("=", 1) for l in state.read_text().splitlines()) if state.exists() else {}
        calls = log.read_text().splitlines() if log.exists() else []
        return st, calls

    def test_stick_without_modules(self):
        st, calls = self.run_scan(stick=True, modules=False)
        self.assertEqual(st.get("present"), "1")
        self.assertEqual(st.get("missing"), "scsi_mod sd_mod usb-storage")
        self.assertFalse([c for c in calls if c.startswith("insmod") or c.startswith("mount")], calls)

    def test_stick_mounted_with_modules(self):
        st, calls = self.run_scan(stick=True, modules=True)
        loads = [pathlib.Path(c.split()[1]).stem for c in calls if c.startswith("insmod")]
        self.assertEqual(loads, ["scsi_mod", "sd_mod", "usb-storage"])
        self.assertIn(f"mknod {self.root}/dev/sda1 b 8 1", calls)
        self.assertTrue(any(c.startswith("mount -t vfat -o noatime") and c.endswith("/mnt/usb") for c in calls), calls)
        self.assertEqual(st.get("present"), "1")
        self.assertEqual(st.get("missing"), "")

    def test_no_stick(self):
        st, calls = self.run_scan(stick=False, modules=True)
        self.assertEqual(st.get("present"), "0")
        self.assertFalse([c for c in calls if c.startswith("insmod")], calls)


class UsbStorageApi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_hub_daemon_test()

    def test_api(self):
        with tempfile.TemporaryDirectory() as tmp, HubHarness(port=PORT, env={"HARMONY_TEST_TMP_DIR": tmp}) as hub:
            st, r = hub.http_get("/api/usb-storage")
            self.assertEqual(st, 200)
            self.assertFalse(r["present"])
            self.assertFalse(r["busy"])

            with open(os.path.join(tmp, "usb_storage_state"), "w") as f:
                f.write("present=1\ndevice=sda1\nfs=vfat\nmounted=1\nmissing=\ntotal_kb=7800000\nfree_kb=7799900\noffload=0\n")
            st, r = hub.http_get("/api/usb-storage")
            self.assertEqual((r["present"], r["mounted"], r["device"], r["fs"]), (True, True, "sda1", "vfat"))
            self.assertEqual((r["total_kb"], r["free_kb"], r["missing"], r["offload"]), (7800000, 7799900, [], False))

            for bad in ("format", "eject;reboot", ""):
                st, r = hub.http_post("/api/usb-storage", {"action": bad})
                self.assertEqual(st, 400, bad)
            st, r = hub.http_post("/api/usb-storage", {"action": "offload"})
            self.assertEqual((st, r.get("action")), (202, "offload"))
            st, r = hub.http_get("/api/usb-storage")
            self.assertTrue(r["busy"])
            st, r = hub.http_post("/api/usb-storage", {"action": "eject"})
            self.assertEqual(st, 409)

            os.unlink(os.path.join(tmp, "usb_storage_running"))
            with open(os.path.join(tmp, "usb_storage.rc"), "w") as f:
                f.write("1\n")
            with open(os.path.join(tmp, "usb_storage.log"), "w") as f:
                f.write("[-] Could not copy the icon archive to the stick\n\n")
            st, r = hub.http_get("/api/usb-storage")
            self.assertFalse(r["busy"])
            self.assertTrue(r.get("failed"))
            self.assertEqual(r["message"], "[-] Could not copy the icon archive to the stick")


if __name__ == "__main__":
    unittest.main(verbosity=2)
