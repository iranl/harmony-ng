#!/usr/bin/env python3
"""USB stick support for non-essential files.

payload/scripts/usb_storage.sh scan in a host sandbox (fake sysfs, insmod/mount stubs):
- a stick behind a USB hub (interface class 08) without the storage modules installed is
  reported present with the missing modules, and nothing is loaded or mounted;
- with the modules installed they are loaded in order (scsi_mod, sd_mod, usb-storage), the
  partition gets a /dev node and is mounted on /mnt/usb;
- no stick: nothing is reported present;
- offload with a mounted stick copies the icons and the plugins (with subfolders) to it, removes
  the flash copies and bind-mounts the plugin folder from the stick.

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

    def run_scan(self, stick: bool, modules: bool, action: str = "scan", setup=None, df_avail: int = 7799900):
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
        for d in ("tmp", "dev", "mnt", "proc", "cache/modules", "data/harmony/config", "data/harmony/custom_icons"):
            (root / d).mkdir(parents=True, exist_ok=True)
        # a fake /proc as well: mounts, and no plugin process of the host is ever signalled
        (root / "proc" / "mounts").write_text("")
        if setup:
            setup(root)
        if modules:
            for m in ("scsi_mod", "sd_mod", "usb-storage"):
                (root / "cache" / "modules" / f"{m}.ko").write_text("")
        log = root / "calls.log"
        stub = root / "stub"
        stub.mkdir()
        (stub / "busybox").write_text(
            "#!/bin/sh\n"
            f"echo \"$*\" >> '{log}'\n"
            'case "$1" in\n'
            f'  df) echo "Filesystem 1K-blocks Used Available Use% Mounted on"; echo "/dev/sda1 7800000 100 {df_avail} 1% /x";;\n'
            '  du|ls) exec "$@";;\n'
            "esac\n"
            "exit 0\n")
        (stub / "insmod").write_text(f"#!/bin/sh\necho \"insmod $1\" >> '{log}'\n")
        (stub / "sleep").write_text("#!/bin/sh\nexit 0\n")
        for p in stub.iterdir():
            p.chmod(0o755)
        text = SCRIPT.read_text(encoding="utf-8").replace("BB=/cache/bin/busybox\n", "BB=@STUB@\n")
        text = text.replace('[ -x "$BB" ] || BB=busybox\n', "")
        text = re.sub(r"(?<![\w./$-])/(data|sys|cache|tmp|dev|mnt|proc)(?=[/ \n])", lambda m: f"{root}/{m.group(1)}", text)
        text = text.replace("@STUB@", f"{stub}/busybox")
        script = root / "usb_storage.sh"
        script.write_text(text)
        env = dict(os.environ, PATH=f"{stub}:/usr/bin:/bin")
        self.out = subprocess.run(["sh", str(script), action], env=env, capture_output=True, text=True, timeout=30)
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

    def test_offload_moves_icons_and_plugins(self):
        def setup(root):
            (root / "proc" / "mounts").write_text(f"/dev/sda1 {root}/mnt/usb vfat rw,noatime 0 0\n")
            (root / "mnt" / "usb").mkdir(parents=True, exist_ok=True)
            (root / "cache" / "icons.tar.gz").write_bytes(b"archive")
            (root / "data" / "harmony" / "custom_icons" / "tv.png").write_bytes(b"png")
            plug = root / "data" / "harmony" / "plugins" / "roku-ecp"
            (plug / "bin").mkdir(parents=True)
            (plug / "manifest.json").write_text("{}")
            (plug / "bin" / "roku").write_bytes(b"\x7fELF")
            (plug / ".source").write_text("x")
        st, calls = self.run_scan(stick=True, modules=True, action="offload", setup=setup)
        usb = self.root / "mnt" / "usb" / "harmony"
        self.assertIn("Icons and plugins moved", self.out.stdout, self.out.stdout + self.out.stderr)
        self.assertEqual((usb / "plugins" / "roku-ecp" / "bin" / "roku").read_bytes(), b"\x7fELF")
        self.assertTrue((usb / "plugins" / "roku-ecp" / ".source").exists())
        self.assertEqual((usb / "icons.tar.gz").read_bytes(), b"archive")
        self.assertEqual(list((self.root / "data" / "harmony" / "plugins").iterdir()), [])
        self.assertFalse((self.root / "cache" / "icons.tar.gz").exists())
        self.assertTrue((self.root / "data" / "harmony" / "config" / "usb_offload").exists())
        self.assertIn(f"mount -o bind {usb}/plugins {self.root}/data/harmony/plugins", calls)
        self.assertEqual(st.get("offload"), "1")
        # the flag and the stick carry the same id: only this stick is used
        flag_id = (self.root / "data" / "harmony" / "config" / "usb_offload").read_text().strip()
        self.assertTrue(flag_id)
        self.assertEqual((usb / ".offload_id").read_text().strip(), flag_id)

    # a stick the icons and plugins were moved to
    def offloaded(self, root, plugins=("roku-ecp",), mounts_extra=""):
        (root / "proc" / "mounts").write_text(f"/dev/sda1 {root}/mnt/usb vfat rw,noatime 0 0\n" + mounts_extra)
        usb = root / "mnt" / "usb" / "harmony"
        (usb / "custom_icons").mkdir(parents=True)
        (usb / "custom_icons" / "tv.png").write_bytes(b"png")
        (usb / "icons.tar.gz").write_bytes(b"archive")
        for p in plugins:
            (usb / "plugins" / p / "bin").mkdir(parents=True)
            (usb / "plugins" / p / "manifest.json").write_text("{}")
            (usb / "plugins" / p / "bin" / "x").write_bytes(b"\x7fELF" + p.encode())
        (usb / ".offload_id").write_text("id-1\n")
        (root / "data" / "harmony" / "config" / "usb_offload").write_text("id-1\n")
        return usb

    def test_offload_keeps_plugins_only_on_the_stick(self):
        # restore without the stick, then offload again: the plugins on the stick must stay
        def setup(root):
            self.usb = self.offloaded(root, plugins=("roku-ecp",))
            # what a restore without the stick leaves: no flag, its id kept
            (root / "data" / "harmony" / "config" / "usb_offload").rename(
                root / "data" / "harmony" / "config" / "usb_offload.last")
            plug = root / "data" / "harmony" / "plugins" / "android-tv"
            plug.mkdir(parents=True)
            (plug / "manifest.json").write_text("{}")
        self.run_scan(stick=True, modules=True, action="offload", setup=setup)
        self.assertEqual(self.out.returncode, 0, self.out.stdout + self.out.stderr)
        self.assertTrue((self.usb / "plugins" / "roku-ecp" / "bin" / "x").exists())
        self.assertTrue((self.usb / "plugins" / "android-tv" / "manifest.json").exists())

    def test_restore_copies_back_and_merges(self):
        def setup(root):
            self.offloaded(root, plugins=("roku-ecp",))
            # a plugin installed on flash while the stick was out (hidden below the mount)
            plug = root / "data" / "harmony" / "plugins" / "android-tv"
            plug.mkdir(parents=True)
            (plug / "manifest.json").write_text("{}")
        self.run_scan(stick=True, modules=True, action="restore", setup=setup)
        data = self.root / "data" / "harmony"
        self.assertIn("kept on flash again", self.out.stdout, self.out.stdout + self.out.stderr)
        self.assertEqual((data / "plugins" / "roku-ecp" / "bin" / "x").read_bytes(), b"\x7fELFroku-ecp")
        self.assertTrue((data / "plugins" / "android-tv" / "manifest.json").exists())
        self.assertEqual((data / "custom_icons" / "tv.png").read_bytes(), b"png")
        self.assertEqual((self.root / "cache" / "icons.tar.gz").read_bytes(), b"archive")
        self.assertFalse((data / "config" / "usb_offload").exists())
        self.assertFalse((data / "plugins.restore").exists())
        # a move: nothing of it stays on the stick to come back with the next offload
        usb = self.root / "mnt" / "usb" / "harmony"
        self.assertEqual(sorted(p.name for p in usb.iterdir()), [])

    def test_offload_refuses_folders_of_another_hub(self):
        # no flag and no kept id on this Hub: the stick's folders are not this Hub's
        def setup(root):
            self.usb = self.offloaded(root, plugins=("otherhub",))
            (root / "data" / "harmony" / "config" / "usb_offload").unlink()
            plug = root / "data" / "harmony" / "plugins" / "roku-ecp"
            plug.mkdir(parents=True)
            (plug / "manifest.json").write_text("{}")
        self.run_scan(stick=True, modules=True, action="offload", setup=setup)
        self.assertNotEqual(self.out.returncode, 0)
        self.assertIn("not moved there from this Hub", self.out.stdout)
        self.assertTrue((self.root / "data" / "harmony" / "plugins" / "roku-ecp" / "manifest.json").exists())
        self.assertFalse((self.usb / "plugins" / "roku-ecp").exists())
        self.assertEqual((self.usb / ".offload_id").read_text().strip(), "id-1")
        self.assertFalse((self.root / "data" / "harmony" / "config" / "usb_offload").exists())

    def test_restore_without_stick_keeps_the_id(self):
        def setup(root):
            (root / "data" / "harmony" / "config" / "usb_offload").write_text("id-1\n")
        self.run_scan(stick=False, modules=True, action="restore", setup=setup)
        cfg = self.root / "data" / "harmony" / "config"
        self.assertEqual(self.out.returncode, 0, self.out.stdout + self.out.stderr)
        self.assertFalse((cfg / "usb_offload").exists())
        self.assertEqual((cfg / "usb_offload.last").read_text().strip(), "id-1")

    def test_restore_without_flag_does_nothing(self):
        # restore with offload off: the stick's folders must not replace the flash plugins
        def setup(root):
            self.offloaded(root, plugins=("roku-ecp",))
            (root / "data" / "harmony" / "config" / "usb_offload").unlink()
            plug = root / "data" / "harmony" / "plugins" / "roku-ecp"
            plug.mkdir(parents=True)
            (plug / "manifest.json").write_text('{"version":"2.0"}')
        self.run_scan(stick=True, modules=True, action="restore", setup=setup)
        self.assertEqual(self.out.returncode, 0)
        self.assertIn("nothing to restore", self.out.stdout)
        self.assertEqual((self.root / "data" / "harmony" / "plugins" / "roku-ecp" / "manifest.json").read_text(),
                         '{"version":"2.0"}')

    def test_leftovers_of_a_killed_restore_are_removed(self):
        def setup(root):
            left = root / "data" / "harmony" / "plugins.restore" / "big"
            left.mkdir(parents=True)
            (left / "data.bin").write_bytes(b"x" * 1024)
        self.run_scan(stick=False, modules=True, setup=setup)
        self.assertFalse((self.root / "data" / "harmony" / "plugins.restore").exists())

    def test_lock_of_a_dead_run_is_taken_over(self):
        def setup(root):
            (root / "tmp" / "usb_storage.lock").mkdir()
            (root / "tmp" / "usb_storage.lock" / "pid").write_text("999999\n")
        st, _ = self.run_scan(stick=False, modules=True, setup=setup)
        self.assertEqual(self.out.returncode, 0, self.out.stdout + self.out.stderr)
        self.assertEqual(st.get("present"), "0")
        self.assertFalse((self.root / "tmp" / "usb_storage.lock").exists())

    def test_failed_offload_can_be_retried(self):
        # the first offload stops at an icon (the stick refuses it) after the icon archive; the id
        # is already on the stick and kept on flash, so the retry goes on with what it left
        def setup(root):
            (root / "proc" / "mounts").write_text(f"/dev/sda1 {root}/mnt/usb vfat rw,noatime 0 0\n")
            usb = root / "mnt" / "usb" / "harmony"
            (usb / "custom_icons").mkdir(parents=True)
            (usb / "custom_icons").chmod(0o555)
            (root / "data" / "harmony" / "custom_icons" / "tv.png").write_bytes(b"png")
            (root / "cache" / "icons.tar.gz").write_bytes(b"archive")
            plug = root / "data" / "harmony" / "plugins" / "roku-ecp"
            plug.mkdir(parents=True)
            (plug / "manifest.json").write_text("{}")
        self.run_scan(stick=True, modules=True, action="offload", setup=setup)
        usb = self.root / "mnt" / "usb" / "harmony"
        cfg = self.root / "data" / "harmony" / "config"
        self.assertNotEqual(self.out.returncode, 0, self.out.stdout)
        kept = (cfg / "usb_offload.last").read_text().strip()
        self.assertTrue(kept)
        self.assertEqual((usb / ".offload_id").read_text().strip(), kept)
        self.assertFalse((cfg / "usb_offload").exists())
        self.assertEqual((self.root / "data" / "harmony" / "custom_icons" / "tv.png").read_bytes(), b"png")
        self.assertTrue((usb / "icons.tar.gz").exists())
        (usb / "custom_icons").chmod(0o755)
        (usb / "custom_icons" / "half.png").write_bytes(b"x")  # what the run left in the folder
        self.out = subprocess.run(["sh", str(self.root / "usb_storage.sh"), "offload"],
                                  env=dict(os.environ, PATH=f"{self.root / 'stub'}:/usr/bin:/bin"),
                                  capture_output=True, text=True, timeout=30)
        self.assertEqual(self.out.returncode, 0, self.out.stdout + self.out.stderr)
        self.assertEqual((cfg / "usb_offload").read_text().strip(), kept)
        self.assertEqual((usb / "custom_icons" / "tv.png").read_bytes(), b"png")
        self.assertFalse((cfg / "usb_offload.last").exists())

    def test_kept_id_not_given_to_another_stick(self):
        # after a restore without stick A, a blank stick B gets its own id: A is not bound later
        def setup(root):
            (root / "proc" / "mounts").write_text(f"/dev/sda1 {root}/mnt/usb vfat rw,noatime 0 0\n")
            (root / "mnt" / "usb").mkdir(parents=True)
            (root / "data" / "harmony" / "config" / "usb_offload.last").write_text("id-a\n")
            plug = root / "data" / "harmony" / "plugins" / "newer"
            plug.mkdir(parents=True)
            (plug / "manifest.json").write_text("{}")
        self.run_scan(stick=True, modules=True, action="offload", setup=setup)
        self.assertEqual(self.out.returncode, 0, self.out.stdout + self.out.stderr)
        stick_id = (self.root / "mnt" / "usb" / "harmony" / ".offload_id").read_text().strip()
        self.assertNotEqual(stick_id, "id-a")
        self.assertEqual((self.root / "data" / "harmony" / "config" / "usb_offload").read_text().strip(), stick_id)

    def test_lock_with_an_empty_pid_file_is_taken_over(self):
        def setup(root):
            (root / "tmp" / "usb_storage.lock").mkdir()
            (root / "tmp" / "usb_storage.lock" / "pid").write_text("")
        st, _ = self.run_scan(stick=False, modules=True, setup=setup)
        self.assertEqual(self.out.returncode, 0, self.out.stdout + self.out.stderr)
        self.assertEqual(st.get("present"), "0")
        self.assertFalse((self.root / "tmp" / "usb_storage.lock").exists())

    def test_room_check_counts_file_sizes(self):
        # 100 icons of 3 KB: 400 KB with 1 KB per file, far less than whole FAT clusters
        def setup(root):
            usb = self.offloaded(root, plugins=())
            for i in range(100):
                (usb / "custom_icons" / f"i{i}.png").write_bytes(b"x" * 3000)
        self.run_scan(stick=True, modules=True, action="restore", setup=setup, df_avail=660)
        self.assertEqual(self.out.returncode, 0, self.out.stdout + self.out.stderr)
        self.assertEqual(len(list((self.root / "data" / "harmony" / "custom_icons").iterdir())), 101)

    def test_restore_without_room_changes_nothing(self):
        def setup(root):
            usb = self.offloaded(root, plugins=("roku-ecp", "big"))
            (usb / "plugins" / "big" / "data.bin").write_bytes(b"x" * 300 * 1024)
        self.run_scan(stick=True, modules=True, action="restore", setup=setup, df_avail=400)
        data = self.root / "data" / "harmony"
        self.assertNotEqual(self.out.returncode, 0)
        self.assertIn("No room on flash", self.out.stdout)
        self.assertFalse((data / "plugins").exists() and list((data / "plugins").iterdir()))
        self.assertEqual(list((data / "custom_icons").iterdir()), [])
        self.assertFalse((self.root / "cache" / "icons.tar.gz").exists())
        self.assertTrue((data / "config" / "usb_offload").exists())

    def test_restore_with_busy_icon_folder_fails(self):
        # the fake umount never unmounts: the custom icon folder stays bound to the stick
        def setup(root):
            self.offloaded(root, mounts_extra=f"/dev/sda1 {root}/data/harmony/custom_icons vfat rw 0 0\n")
        self.run_scan(stick=True, modules=True, action="restore", setup=setup)
        self.assertNotEqual(self.out.returncode, 0)
        self.assertIn("custom icon folder on the stick is still in use", self.out.stdout)
        self.assertTrue((self.root / "data" / "harmony" / "config" / "usb_offload").exists())

    def other_stick(self, root):
        self.offloaded(root)
        (root / "mnt" / "usb" / "harmony" / ".offload_id").write_text("id-other\n")

    def test_other_stick_is_not_bound(self):
        st, calls = self.run_scan(stick=True, modules=True, setup=self.other_stick)
        self.assertFalse([c for c in calls if c.startswith("mount -o bind")], calls)
        self.assertEqual(st.get("other_stick"), "1")

    def test_other_stick_is_not_restored(self):
        self.run_scan(stick=True, modules=True, action="restore", setup=self.other_stick)
        self.assertNotEqual(self.out.returncode, 0)
        self.assertEqual(list((self.root / "data" / "harmony" / "custom_icons").iterdir()), [])
        self.assertTrue((self.root / "data" / "harmony" / "config" / "usb_offload").exists())


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
