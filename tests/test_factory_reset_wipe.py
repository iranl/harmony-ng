#!/usr/bin/env python3
"""One factory reset script per device, and it deletes the secrets.

The scripts run with sh in a sandbox: every absolute device path is moved under a temporary
root (a check makes sure none is left), and killall, mount, reboot, sleep and sync are stubs
that record their calls. Nothing outside the temporary directory is touched. As on the Hub,
/mnt/root (the stock file system) cannot be read: it is under the union mounted over /. The
mount stub models mounting it again: "mount -t squashfs" copies a folder of stock files to the
mount point when the test provides one (and fails otherwise).

- payload/scripts/factory_reset.sh (Hub) deletes /data/harmony (accounts, tokens, web UI
  login, MQTT settings, Bluetooth keys and host files, devices), resets the Wi-Fi file to the
  stock firmware's copy (or to one without a network), removes SSH access, and reboots only
  without --no-reboot;
- remote/factory_reset.sh deletes the remote's settings, the Hub's names, logs and SSH keys,
  keeps the stock /data/resources, and reboots only without --no-reboot;
- every other way to reset runs these scripts: the remote's Settings menu has no copy of its
  own, tools/factory_reset.py, deploy_remote.py --factory-reset and setup_remote.py
  --factory-reset send the script itself to the device.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
HUB_SCRIPT = REPO_DIR / "payload" / "scripts" / "factory_reset.sh"
REMOTE_SCRIPT = REPO_DIR / "remote" / "factory_reset.sh"
TOP = "tmp|data|etc|cache|usr|opt|mnt|root|home|bin|sbin|lib|proc|sys"
# absolute paths left as they are: output redirection and text written into a file
ALLOWED = ("/dev/null", "/var/run/wpa_supplicant", "/dev/mspfw", "/dev/clearpad")
PATH_RE = re.compile(r"(?<![\w./$!-])/(" + TOP + r")(?=[/\s\"';.,)]|$)", re.M)


class Sandbox:
    def __init__(self, script: pathlib.Path) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        self.root = self.base / "root"
        self.root.mkdir()
        (self.root / "tmp").mkdir()
        stub = self.base / "stub"
        stub.mkdir()
        self.calls = self.base / "calls.log"
        for cmd in ("killall", "reboot", "sleep", "sync"):
            f = stub / cmd
            f.write_text(f"#!/bin/sh\necho \"{cmd} $*\" >> '{self.calls}'\nexit 0\n")
            f.chmod(0o755)
        self.stock = self.base / "stock"  # the stock file system, as the mount stub shows it
        mount = stub / "mount"
        # like the kernel: the same file system twice on one mount point is refused
        mount.write_text(f"""#!/bin/sh
echo "mount $*" >> '{self.calls}'
if [ "$1" = "-t" ] && [ "$2" = squashfs ]; then
  for last; do :; done
  [ -d '{self.stock}' ] || exit 32
  [ -e "$last/.mounted" ] && exit 255
  cp -a '{self.stock}/.' "$last/" || exit 32
  : > "$last/.mounted"
fi
exit 0
""")
        mount.chmod(0o755)
        umount = stub / "umount"
        umount.write_text(f"""#!/bin/sh
echo "umount $*" >> '{self.calls}'
[ -e "$1/.mounted" ] || exit 1
rm -rf "$1"/* "$1/.mounted"
exit 0
""")
        umount.chmod(0o755)
        # what the Hub's /proc/mounts says (paths as the sandboxed script sees them)
        self.file("proc/mounts", f"/dev/mtdblock5 {self.root}/cache jffs2 rw 0 0\n"
                  f"/dev/mtdblock3 {self.root}/mnt/root squashfs ro,relatime 0 0\n"
                  f"/dev/mtdblock4 {self.root}/mnt/data jffs2 rw,relatime 0 0\n"
                  f"none / unionfs rw,dirs={self.root}/mnt/data=rw:{self.root}/mnt/root=ro 0 0\n")
        text = PATH_RE.sub(lambda m: f"{self.root}{m.group(0)}", script.read_text())
        rest = text.replace(str(self.root), "ROOT")
        for allowed in ALLOWED:
            rest = rest.replace(allowed, "")
        leftover = re.findall(r"(?<![\w./$!-])/[A-Za-z]\w*", rest)
        if leftover:
            raise AssertionError(f"device paths outside the sandbox: {leftover}")
        self.script = self.base / script.name
        self.script.write_text(text)
        self.env = dict(os.environ, PATH=f"{stub}:/usr/bin:/bin")

    def stock_file(self, rel: str, text: str) -> None:
        p = self.stock / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def file(self, rel: str, text: str = "x\n") -> pathlib.Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return p

    def run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["sh", str(self.script), *args], env=self.env, capture_output=True, text=True,
                              timeout=60)

    def call_log(self) -> list[str]:
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def close(self) -> None:
        self.tmp.cleanup()


HUB_SECRETS = [
    "data/harmony/config/users.json", "data/harmony/config/tokens.json", "data/harmony/webui_auth.conf",
    "data/harmony/mqtt.json", "data/harmony/btstack_keys.tlv", "data/harmony/bthid_host_target",
    "data/harmony/bt_remote_target", "data/harmony/bt-devices.json", "data/harmony/config/devices.json",
    "data/harmony/config/activities.json", "data/harmony/remote_slot1_map.json",
    "etc/dropbear/authorized_keys", "etc/dropbear/dropbear_rsa_host_key", "etc/dropbear/dropbear_ed25519_host_key",
    "root/.ssh/authorized_keys", "home/root/.ssh/authorized_keys", "data/dropbearmulti", "usr/sbin/dropbear",
    "etc/wpa_supplicant.ath0.conf", "cache/www/index.html", "data/preserved_config/mqtt.json",
]


class HubScript(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox(HUB_SCRIPT)
        for rel in HUB_SECRETS:
            self.s.file(rel)
        self.s.file("etc/wpa_supplicant.conf", 'network={\n\tssid="home"\n\tpsk="secret"\n}\n')
        self.s.file("etc/init.d/rcS.local", "#!/bin/sh\n# harmony-ng\n")
        self.s.file("data/stock_backup/rcS.local.orig", "#!/bin/sh\n# stock\n")
        self.s.file("data/resources/Context.json", "{}\n")  # the stock firmware's own data

    def tearDown(self):
        self.s.close()

    def test_wipes_secrets_and_keeps_stock_data(self):
        self.s.stock_file("etc/wpa_supplicant.conf", "ctrl_interface=/var/run/wpa_supplicant\n# stock\n")
        res = self.s.run("--confirm")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        for rel in HUB_SECRETS:
            self.assertFalse((self.s.root / rel).exists(), f"{rel} is still there")
        self.assertFalse((self.s.root / "data" / "harmony").exists(), "/data/harmony is gone")
        self.assertEqual((self.s.root / "etc" / "wpa_supplicant.conf").read_text(),
                         "ctrl_interface=/var/run/wpa_supplicant\n# stock\n", "Wi-Fi file = the stock copy")
        self.assertTrue((self.s.root / "data" / "resources" / "Context.json").exists())
        self.assertIn("# stock", (self.s.root / "etc" / "init.d" / "rcS.local").read_text())
        log = self.s.call_log()
        self.assertIn("reboot -f", log)
        mounted = [c for c in log if c.startswith("mount -t squashfs")]
        self.assertEqual(len(mounted), 1, log)
        self.assertIn("/dev/mtdblock3", mounted[0], "the device from /proc/mounts")
        last_umount = [i for i, c in enumerate(log) if c.startswith("umount")][-1]
        self.assertLess(log.index(mounted[0]), last_umount)
        self.assertLess(last_umount, log.index("reboot -f"))
        self.assertFalse((self.s.root / "tmp" / "harmony_stock_root").exists(), "the mount point is removed")

    def test_stock_boot_files_come_back(self):
        # harmony-ng's rcS.local took ath0 out of "auto": the stock firmware's DHCP needs it back
        self.s.file("etc/network/interfaces", "auto lo\niface ath0 inet dhcp\n")
        self.s.stock_file("etc/network/interfaces", "auto lo ath0\niface ath0 inet dhcp\n# stock\n")
        self.s.stock_file("etc/init.d/rcS.local", "#!/bin/sh\n# factory\n")
        self.s.stock_file("usr/sbin/scp", "stock scp\n")  # a path harmony-ng also writes
        self.s.file("usr/sbin/scp", "link to dropbearmulti\n")
        res = self.s.run("--confirm", "--no-reboot")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual((self.s.root / "etc" / "network" / "interfaces").read_text(),
                         "auto lo ath0\niface ath0 inet dhcp\n# stock\n")
        self.assertIn("# factory", (self.s.root / "etc" / "init.d" / "rcS.local").read_text(),
                      "the stock firmware's own copy first")
        self.assertEqual((self.s.root / "usr" / "sbin" / "scp").read_text(), "stock scp\n",
                         "a stock file is put back, not deleted")

    def test_stock_file_system_not_readable(self):
        # no stock copies: rcS.local from the copy saved at the first install, interfaces edited
        # back, nothing the stock firmware has is deleted
        self.s.file("etc/network/interfaces", "auto lo\niface ath0 inet dhcp\n")
        res = self.s.run("--confirm", "--no-reboot")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual((self.s.root / "etc" / "network" / "interfaces").read_text(),
                         "auto lo ath0\niface ath0 inet dhcp\n")
        self.assertIn("# stock", (self.s.root / "etc" / "init.d" / "rcS.local").read_text())
        self.assertTrue((self.s.root / "etc" / "wpa_supplicant.conf").exists())
        # a host key the stock image may have is left; the one the installers made is removed
        self.assertTrue((self.s.root / "etc" / "dropbear" / "dropbear_rsa_host_key").exists())
        self.assertFalse((self.s.root / "etc" / "dropbear" / "dropbear_ed25519_host_key").exists())
        self.assertFalse((self.s.root / "etc" / "dropbear" / "authorized_keys").exists())

    def test_mount_left_by_a_cut_run_is_cleared_first(self):
        self.s.stock_file("etc/network/interfaces", "auto lo ath0\n# stock\n")
        self.s.file("etc/network/interfaces", "auto lo\n")
        self.s.file("tmp/harmony_stock_root/.mounted", "")
        res = self.s.run("--confirm", "--no-reboot")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual((self.s.root / "etc" / "network" / "interfaces").read_text(), "auto lo ath0\n# stock\n")
        self.assertFalse((self.s.root / "tmp" / "harmony_stock_root").exists())

    def test_cut_login_does_not_stop_the_reset(self):
        self.s.stock_file("etc/network/interfaces", "auto lo ath0\n# stock\n")
        self.s.stock_file("etc/dropbear/dropbear_rsa_host_key", "stock host key\n")
        self.s.file("etc/network/interfaces", "auto lo\n")
        # the output closes after the first line and SIGHUP arrives, as when the SSH login is cut
        import signal
        proc = subprocess.Popen(["sh", str(self.s.script), "--confirm", "--no-reboot"], env=self.s.env,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        proc.stdout.readline()
        proc.stdout.close()
        proc.send_signal(signal.SIGHUP)
        self.assertEqual(proc.wait(timeout=60), 0, "the reset went on to its end")
        self.assertEqual((self.s.root / "etc" / "network" / "interfaces").read_text(), "auto lo ath0\n# stock\n")
        self.assertIn("# stock", (self.s.root / "etc" / "init.d" / "rcS.local").read_text())
        self.assertEqual((self.s.root / "etc" / "dropbear" / "dropbear_rsa_host_key").read_text(), "stock host key\n")
        self.assertFalse((self.s.root / "data" / "harmony").exists())
        self.assertFalse((self.s.root / "tmp" / "harmony_stock_root").exists(), "the stock file system is unmounted")

    def test_interrupted_run_unmounts(self):
        import signal
        self.s.stock_file("etc/network/interfaces", "auto lo ath0\n")
        chmod = self.s.base / "stub" / "chmod"  # a slow step while the stock file system is mounted
        chmod.write_text("#!/bin/sh\n/bin/sleep 2\nexec /bin/chmod \"$@\"\n")
        chmod.chmod(0o755)
        proc = subprocess.Popen(["sh", str(self.s.script), "--confirm", "--no-reboot"], env=self.s.env,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        proc.stdout.readline()
        for _ in range(50):
            if (self.s.root / "tmp" / "harmony_stock_root" / ".mounted").exists():
                break
            time.sleep(0.05)
        proc.send_signal(signal.SIGTERM)
        self.assertNotEqual(proc.wait(timeout=60), 0)
        self.assertFalse((self.s.root / "tmp" / "harmony_stock_root" / ".mounted").exists(), "unmounted")

    def test_failed_write_leaves_the_boot_script_whole(self):
        (self.s.root / "etc" / "init.d" / "rcS.local.new").mkdir()  # cp to rcS.local.new fails
        res = self.s.run("--confirm", "--no-reboot")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("was not changed", res.stdout)
        self.assertIn("# harmony-ng", (self.s.root / "etc" / "init.d" / "rcS.local").read_text())
        self.assertTrue((self.s.root / "data" / "harmony" / "config" / "users.json").exists())

    def test_no_stock_boot_script_anywhere_changes_nothing(self):
        (self.s.root / "data" / "stock_backup" / "rcS.local.orig").unlink()
        # init.sh records the signals it ignores: the services start as at boot
        sig = self.s.base / "init_sigign"
        init = self.s.file("data/harmony/init.sh", f"#!/bin/sh\ngrep SigIgn /proc/$$/status > '{sig}'\n")
        init.chmod(0o755)
        res = self.s.run("--confirm", "--no-reboot")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("# harmony-ng", (self.s.root / "etc" / "init.d" / "rcS.local").read_text(),
                      "not deleted: the union would hide the stock file")
        self.assertTrue((self.s.root / "data" / "harmony" / "config" / "users.json").exists())
        for _ in range(100):
            if sig.exists() and sig.read_text().strip():
                break
            time.sleep(0.05)
        mask = int(sig.read_text().split()[-1], 16)
        self.assertEqual(mask & 0x1001, 0, "init.sh starts without SIGHUP and SIGPIPE ignored")
        self.assertFalse((self.s.root / "tmp" / "factory_reset.lock").exists(), "the lock is released")

    def test_one_reset_at_a_time(self):
        other = subprocess.Popen(["sleep", "30"])
        try:
            self.s.file("tmp/factory_reset.lock/pid", f"{other.pid}\n")
            res = self.s.run("--confirm", "--no-reboot")
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("Another factory reset is running", res.stdout)
            self.assertTrue((self.s.root / "data" / "harmony" / "config" / "users.json").exists())
        finally:
            other.kill()
            other.wait()
        # a lock left by a run that no longer exists is taken over
        res = self.s.run("--confirm", "--no-reboot")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertFalse((self.s.root / "data" / "harmony").exists())
        self.assertFalse((self.s.root / "tmp" / "factory_reset.lock").exists())

    def test_secrets_go_before_the_ways_in(self):
        text = HUB_SCRIPT.read_text()
        self.assertLess(text.index("rm -rf /data/harmony"), text.index("restore_stock etc/wpa_supplicant.conf"))
        self.assertLess(text.index("restore_stock etc/wpa_supplicant.conf"), text.index('"etc/dropbear/$_k"'))

    def test_no_stock_path_is_deleted_directly(self):
        text = HUB_SCRIPT.read_text()
        for path in ("/etc/network/interfaces", "/etc/init.d/rcS.local", "/etc/wpa_supplicant.conf"):
            self.assertNotRegex(text, r"rm -f[^\n]*" + re.escape(path) + r"(\s|$)")
        self.assertNotIn("/mnt/root/etc", text, "/mnt/root cannot be read on the Hub")

    def test_wifi_file_without_stock_copy_has_no_network(self):
        res = self.s.run("--confirm")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        wpa = (self.s.root / "etc" / "wpa_supplicant.conf").read_text()
        self.assertNotIn("network", wpa)
        self.assertNotIn("secret", wpa)
        self.assertIn("ctrl_interface", wpa)

    def test_no_reboot(self):
        res = self.s.run("--confirm", "--no-reboot")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertNotIn("reboot -f", self.s.call_log())
        self.assertFalse((self.s.root / "data" / "harmony").exists())

    def test_unknown_option_changes_nothing(self):
        res = self.s.run("--confirm", "--keep")
        self.assertNotEqual(res.returncode, 0)
        self.assertTrue((self.s.root / "data" / "harmony" / "config" / "users.json").exists())


REMOTE_SECRETS = [
    "data/remote_settings.conf", "data/remote_rf_config.json", "data/remote_activity_usage.conf",
    "data/remote_ui.bak", "data/remote_flightlog.txt", "data/remote_ui.conf", "data/elite_rf_config.json",
    "data/crash.log", "data/harmony_crash.log", "data/last_kmsg.log", "data/flightlog_on",
    "data/debug_logging.conf", "data/rcS.local", "data/bin/dropbear", "data/dropbearmulti",
    "etc/dropbear/authorized_keys", "etc/dropbear/dropbear_rsa_host_key", "root/.ssh/authorized_keys",
    "home/root/.ssh/authorized_keys", "data/remote_ui", "data/remote_sync", "data/init.sh", "data/factory_reset.sh",
]


class RemoteScript(unittest.TestCase):
    def setUp(self):
        self.s = Sandbox(REMOTE_SCRIPT)
        for rel in REMOTE_SECRETS:
            self.s.file(rel)
        self.s.file("etc/init.d/rcS.local", "#!/bin/sh\n/usr/bin/usbeth start &\n")
        self.s.file("data/resources/stock.json", "{}\n")

    def tearDown(self):
        self.s.close()

    def test_wipes_settings_logs_and_keys(self):
        res = self.s.run("--confirm", "--no-reboot")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        for rel in REMOTE_SECRETS:
            self.assertFalse((self.s.root / rel).exists(), f"{rel} is still there")
        self.assertFalse((self.s.root / "data" / "remote_conf_p").exists(), "nothing is set aside for later")
        self.assertTrue((self.s.root / "data" / "resources" / "stock.json").exists(), "stock data kept")
        self.assertIn("flashuboot", (self.s.root / "etc" / "init.d" / "rcS.local").read_text())
        self.assertNotIn("reboot -f", self.s.call_log())

    def test_reboots_without_no_reboot(self):
        res = self.s.run("--confirm")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("reboot -f", self.s.call_log())


TOOL_HELP = subprocess.run([sys.executable, str(REPO_DIR / "tools" / "factory_reset.py"), "--help"],
                           capture_output=True, text=True, timeout=30).stdout

# Fake ssh: logs each login; the login that runs the reset script takes the script on its
# standard input and "deletes the SSH keys": every later login is refused, as on the device
FAKE_SSH = """#!/bin/sh
for last; do :; done
printf '%s\\n' "$last" >> "$FAKE_DIR/log"
if [ -e "$FAKE_DIR/keys_gone" ]; then
  echo "root@device: Permission denied (publickey)." >&2; exit 255
fi
case "$last" in
  "uname -a") echo "Linux device" ;;
  "uname -m") echo "$FAKE_MACHINE" ;;
esac
case "$last" in *"cat > "*) cat > "$FAKE_DIR/uploaded" ;; esac
case "$last" in *--confirm*) touch "$FAKE_DIR/keys_gone" ;; esac
case "$last" in
  *factory-reset-steps-done*)
    # steps that print a mark when they ran: print the marks the device would
    steps=$(printf '%s' "$last" | grep -o 'echo factory-reset-step [0-9]*' | wc -l)
    i=1; while [ "$i" -le "$steps" ]; do echo "factory-reset-step $i"; i=$((i + 1)); done
    echo "factory-reset-steps-done" ;;
esac
exit 0
"""


class WebUiText(unittest.TestCase):
    """The web UI's reset dialog says that the reset deletes, in every language"""

    def test_dialog_warns_instead_of_promising_safety(self):
        html = (REPO_DIR / "android-app" / "index.html").read_text()
        modal = html[html.index('id="resetModalStep1"'):html.index('id="resetModalStep2"')]
        self.assertNotIn("remain safely stored", modal)
        keys = re.findall(r'data-i18n="([^"]+)"', modal)
        self.assertIn("revert_factory_warn_2", keys)
        for lang in ("en", "de", "fr", "nl", "es", "it"):
            text = (REPO_DIR / "android-app" / "src" / "i18n" / f"{lang}.js").read_text()
            for key in keys:
                self.assertRegex(text, rf'\n    "{key}": "', f"{key} missing in {lang}")
            self.assertNotIn('"configs_are_safe"', text)
            front = (REPO_DIR / "frontend" / "src" / "i18n" / f"{lang}.js").read_text()
            self.assertNotIn('"configs_are_safe"', front, "no unused promise left in the new UI either")


class OneScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        (self.base / "bin").mkdir()
        ssh = self.base / "bin" / "ssh"
        ssh.write_text(FAKE_SSH)
        ssh.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{self.base / 'bin'}:{os.environ['PATH']}", FAKE_DIR=str(self.base),
                        HOME=str(self.base))

    def tearDown(self):
        self.tmp.cleanup()

    def log(self) -> list[str]:
        p = self.base / "log"
        return p.read_text().splitlines() if p.exists() else []

    def uploaded(self) -> bytes:
        return (self.base / "uploaded").read_bytes()

    def test_remote_menu_has_no_copy(self):
        src = (REPO_DIR / "payload" / "source" / "remote" / "remote_ui_settings_actions.c").read_text()
        body = src[src.index("static void execute_remote_factory_reset(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("/data/factory_reset.sh --confirm", body)
        for leftover in ("flashuboot", "rm -f", "rm -rf", "chmod +x"):
            self.assertNotIn(leftover, body, "the menu runs the script only")

    def run_tool(self, target: str, machine: str) -> subprocess.CompletedProcess[str]:
        args = [sys.executable, str(REPO_DIR / "tools" / "factory_reset.py"), "--target", target, "--force"]
        if target == "hub":
            args += ["--hub-ip", "192.168.1.20"]
            if "--no-backup" in TOOL_HELP:  # the tool may save a backup of the Hub first
                args.append("--no-backup")
        env = dict(self.env, FAKE_MACHINE=machine)
        return subprocess.run(args, env=env, capture_output=True, text=True, timeout=60)

    def test_tool_sends_the_hub_script(self):
        res = self.run_tool("hub", "mips")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(self.uploaded(), HUB_SCRIPT.read_bytes())
        self.one_reset_login()
        self.assertFalse(any("rm -rf /data/harmony" in c for c in self.log()), "no reset commands of its own")

    def test_tool_sends_the_remote_script(self):
        res = self.run_tool("remote", "armv5tejl")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(self.uploaded(), REMOTE_SCRIPT.read_bytes())
        self.one_reset_login()

    def test_tool_reports_a_reset_that_failed(self):
        (self.base / "bin" / "ssh").write_text(FAKE_SSH.replace('case "$last" in *--confirm*) touch',
                                                                'case "$last" in *--confirm*) exit 1; touch'))
        res = self.run_tool("remote", "armv5tejl")
        self.assertNotEqual(res.returncode, 0)
        self.assertNotIn("completed successfully", res.stdout)

    def login_sandbox(self):
        """The tools' reset login run here: device paths in a test folder, reboot a stub"""
        sys.path.insert(0, str(REPO_DIR / "tools"))
        import factory_reset as tool
        dev = self.base / "dev"
        dev.mkdir(exist_ok=True)
        stub = self.base / "stub"
        stub.mkdir(exist_ok=True)
        (stub / "reboot").write_text(f"#!/bin/sh\necho \"reboot $*\" >> '{dev}/rebooted'\n")
        (stub / "reboot").chmod(0o755)

        def cmd_for(script: bytes) -> str:
            import hashlib
            c = tool.reset_login_cmd(hashlib.md5(script).hexdigest())
            c = c.replace(tool.DEVICE_RESET_SCRIPT, str(dev / "factory_reset.sh"))
            return c.replace(tool.DEVICE_RESET_LOG, str(dev / "factory_reset.log"))
        env = dict(os.environ, PATH=f"{stub}:{os.environ['PATH']}")
        return tool, dev, env, cmd_for

    def test_cut_login_does_not_stop_the_reset(self):
        """The login runs script, mark and reboot with HUP and PIPE ignored and their output in a
        file: when the login is cut (SIGHUP to its process group, output closed) they go on."""
        import signal
        tool, dev, env, cmd_for = self.login_sandbox()
        script = b"#!/bin/sh\necho step one\nsleep 1\necho step two\n"
        proc = subprocess.Popen(["sh", "-c", cmd_for(script)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                env=env, start_new_session=True)
        proc.stdin.write(script)
        proc.stdin.close()
        log = dev / "factory_reset.log"
        for _ in range(200):  # the script has started
            if log.exists() and "step one" in log.read_text():
                break
            time.sleep(0.05)
        os.killpg(proc.pid, signal.SIGHUP)
        proc.stdout.close()
        proc.wait(timeout=10)
        for _ in range(100):
            if (dev / "rebooted").exists():
                break
            time.sleep(0.1)
        text = log.read_text()
        self.assertIn("step two", text, "the script ran to its end")
        self.assertIn(tool.RESET_DONE_MARK, text)
        self.assertTrue((dev / "rebooted").exists(), "and the reboot started")

    def test_cut_or_empty_upload_is_not_run(self):
        tool, dev, env, cmd_for = self.login_sandbox()
        script = b"#!/bin/sh\ntouch '" + str(dev).encode() + b"/ran'\necho done\n"
        for sent in (script[:20], b""):
            res = subprocess.run(["sh", "-c", cmd_for(script)], input=sent, env=env, capture_output=True, timeout=30)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn(b"did not arrive complete", res.stdout)
            self.assertNotIn(tool.RESET_DONE_MARK.encode(), res.stdout)
            self.assertFalse((dev / "ran").exists())
            self.assertFalse((dev / "rebooted").exists(), "no reboot")
        res = subprocess.run(["sh", "-c", cmd_for(script)], input=script, env=env, capture_output=True, timeout=30)
        self.assertEqual(res.returncode, 0, res)
        self.assertTrue((dev / "ran").exists())

    def test_tool_result_handling(self):
        """mark + exit 255 = done; no mark + 255 = may have finished, power-cycle; no mark + 1 = run again"""
        ssh = self.base / "bin" / "ssh"
        cases = {
            "mark255": (0, "reset and reboots"),
            "nomark255": (1, "power-cycle"),
            "nomark1": (1, "Run the tool again"),
        }
        for mode, (rc, text) in cases.items():
            ssh.write_text(f"""#!/bin/sh
for last; do :; done
case "$last" in
  "uname -a") echo "Linux device"; exit 0 ;;
  "uname -m") echo "$FAKE_MACHINE"; exit 0 ;;
esac
cat > /dev/null
case {mode} in
  mark255) echo factory-reset-steps-done; exit 255 ;;
  nomark255) echo "Connection closed" >&2; exit 255 ;;
  nomark1) exit 1 ;;
esac
""")
            res = self.run_tool("remote", "armv5tejl")
            self.assertEqual(res.returncode, rc, (mode, res.stdout))
            self.assertIn(text, res.stdout, mode)
        # without the mark the tool also says what to do when SSH no longer lets it in
        self.assertIn("power-cycle", res.stdout)

    def test_deploy_remote_refuses_an_empty_script(self):
        kit = self.base / "kit"
        kit.mkdir()
        (kit / "deploy_remote.py").write_bytes((REPO_DIR / "remote" / "deploy_remote.py").read_bytes())
        # when deploy_remote.py imports tools/ssh_hostkeys.py, a release bundle ships it in tools/
        hostkeys = REPO_DIR / "tools" / "ssh_hostkeys.py"
        if hostkeys.is_file():
            (kit / "tools").mkdir()
            (kit / "tools" / "ssh_hostkeys.py").write_bytes(hostkeys.read_bytes())
        (kit / "factory_reset.sh").write_text("")
        res = subprocess.run([sys.executable, str(kit / "deploy_remote.py"), "--factory-reset"],
                             input="RESET\n", env=dict(self.env, FAKE_MACHINE="armv5tejl"),
                             capture_output=True, text=True, timeout=60)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("missing or empty", res.stdout)
        self.assertFalse(any("factory_reset.sh" in c for c in self.log()), "nothing sent")

    def one_reset_login(self) -> None:
        """copy, run and reboot in the same login, after the probe; nothing after it"""
        log = self.log()
        self.assertEqual(len(log), 2, log)
        self.assertIn("sh /tmp/factory_reset.sh --confirm --no-reboot", log[1])
        self.assertIn("reboot -f", log[1])

    def test_deploy_remote_sends_the_script(self):
        res = subprocess.run([sys.executable, str(REPO_DIR / "remote" / "deploy_remote.py"), "--factory-reset"],
                             input="RESET\n", env=dict(self.env, FAKE_MACHINE="armv5tejl"),
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(self.uploaded(), REMOTE_SCRIPT.read_bytes())
        self.one_reset_login()
        self.assertFalse(any("rm -f /data/remote_*" in c for c in self.log()), "no reset commands of its own")

    def run_setup_remote(self, lose_piece: bool = False) -> tuple[int, list[str]]:
        """setup_remote.py --factory-reset with the stock web service replaced: its commands run
        here with /tmp moved into the test folder; the reset itself is not run"""
        import importlib.util
        spec = importlib.util.spec_from_file_location("setup_remote", REPO_DIR / "remote" / "setup_remote.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        sent: list[str] = []
        tmp = self.base / "devtmp"
        tmp.mkdir(exist_ok=True)

        def rce(cmd: str, *a, **k) -> None:
            sent.append(cmd)
            if cmd.startswith("(sh /tmp/factory_reset.sh") or (lose_piece and len(sent) == 2):
                return
            # only the device paths the tool writes and reads, not the script's own text
            for path in ("/tmp/factory_reset.sh", "/tmp/frmd5.json"):
                cmd = re.sub(r"(?<=[ >])" + re.escape(path) + r"(?=$|[ ;)])", f"{tmp}/{pathlib.Path(path).name}", cmd)
            subprocess.run(["sh", "-c", cmd], check=True)

        def read_json_val(remote_file: str, key: str, timeout: int = 5) -> str:
            import json
            return json.loads((tmp / pathlib.Path(remote_file).name).read_text()).get(key, "")

        with mock.patch.object(mod, "rce", side_effect=rce), \
             mock.patch.object(mod, "read_json_val", side_effect=read_json_val), \
             mock.patch.object(mod, "probe_remote", return_value=True), \
             mock.patch.object(mod, "detect_remote_usb_state", return_value={}), \
             mock.patch.object(mod, "print_usb_state_banner"), \
             mock.patch("builtins.input", return_value="FACTORY-RESET"), \
             mock.patch.object(sys, "argv", ["setup_remote.py", "--factory-reset"]):
            with self.assertRaises(SystemExit) as cm:
                mod.main()
        self.written = (tmp / "factory_reset.sh").read_bytes() if (tmp / "factory_reset.sh").exists() else b""
        return cm.exception.code, sent

    def test_setup_remote_writes_and_checks_the_script(self):
        code, sent = self.run_setup_remote()
        self.assertEqual(code, 0)
        self.assertEqual(self.written, REMOTE_SCRIPT.read_bytes())
        self.assertIn("sh /tmp/factory_reset.sh --confirm", sent[-1])
        for cmd in sent:
            self.assertNotIn("\n", cmd, "every command is one line")
            self.assertLess(len(cmd), 4000)

    def test_setup_remote_does_not_run_a_damaged_script(self):
        code, sent = self.run_setup_remote(lose_piece=True)
        self.assertNotEqual(code, 0)
        self.assertFalse(any(c.startswith("(sh /tmp/factory_reset.sh") for c in sent), "the reset is not started")


if __name__ == "__main__":
    unittest.main(verbosity=2)
