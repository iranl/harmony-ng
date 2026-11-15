#!/usr/bin/env python3
"""One factory reset script per device, and it deletes the secrets.

The scripts run with sh in a sandbox: every absolute device path is moved under a temporary
root (a check makes sure none is left), and killall, mount, reboot, sleep and sync are stubs
that record their calls. Nothing outside the temporary directory is touched.

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
        stub = self.base / "stub"
        stub.mkdir()
        self.calls = self.base / "calls.log"
        for cmd in ("killall", "mount", "reboot", "sleep", "sync"):
            f = stub / cmd
            f.write_text(f"#!/bin/sh\necho \"{cmd} $*\" >> '{self.calls}'\nexit 0\n")
            f.chmod(0o755)
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
        self.s.file("mnt/root/etc/wpa_supplicant.conf", "ctrl_interface=/var/run/wpa_supplicant\n# stock\n")
        res = self.s.run("--confirm")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        for rel in HUB_SECRETS:
            self.assertFalse((self.s.root / rel).exists(), f"{rel} is still there")
        self.assertFalse((self.s.root / "data" / "harmony").exists(), "/data/harmony is gone")
        self.assertEqual((self.s.root / "etc" / "wpa_supplicant.conf").read_text(),
                         "ctrl_interface=/var/run/wpa_supplicant\n# stock\n", "Wi-Fi file = the stock copy")
        self.assertTrue((self.s.root / "data" / "resources" / "Context.json").exists())
        self.assertIn("# stock", (self.s.root / "etc" / "init.d" / "rcS.local").read_text())
        self.assertIn("reboot -f", self.s.call_log())

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

FAKE_SSH = """#!/bin/sh
for last; do :; done
printf '%s\\n' "$last" >> "$FAKE_DIR/log"
case "$last" in
  "cat > "*) cat > "$FAKE_DIR/uploaded" ;;
  "uname -a") echo "Linux device" ;;
  "uname -m") echo "$FAKE_MACHINE" ;;
  *factory-reset-steps-done*)
    # all steps in one login, each printing a mark when it ran: print the marks the device would
    steps=$(printf '%s' "$last" | grep -o 'echo factory-reset-step [0-9]*' | wc -l)
    i=1; while [ "$i" -le "$steps" ]; do echo "factory-reset-step $i"; i=$((i + 1)); done
    echo "factory-reset-steps-done" ;;
esac
exit 0
"""


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
        self.assertTrue(any("sh /tmp/factory_reset.sh --confirm --no-reboot" in c for c in self.log()), self.log())
        self.assertFalse(any("rm -rf /data/harmony" in c for c in self.log()), "no reset commands of its own")

    def test_tool_sends_the_remote_script(self):
        res = self.run_tool("remote", "armv5tejl")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(self.uploaded(), REMOTE_SCRIPT.read_bytes())
        self.assertTrue(any("sh /tmp/factory_reset.sh --confirm --no-reboot" in c for c in self.log()), self.log())

    def test_deploy_remote_sends_the_script(self):
        res = subprocess.run([sys.executable, str(REPO_DIR / "remote" / "deploy_remote.py"), "--factory-reset"],
                             input="RESET\n", env=dict(self.env, FAKE_MACHINE="armv5tejl"),
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(self.uploaded(), REMOTE_SCRIPT.read_bytes())
        self.assertTrue(any("sh /tmp/factory_reset.sh --confirm --no-reboot" in c for c in self.log()), self.log())
        self.assertFalse(any("rm -f /data/remote_*" in c for c in self.log()), "no reset commands of its own")

    def test_setup_remote_writes_the_script(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("setup_remote", REPO_DIR / "remote" / "setup_remote.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        sent: list[str] = []
        with mock.patch.object(mod, "rce", side_effect=lambda cmd, *a, **k: sent.append(cmd)), \
             mock.patch.object(mod, "probe_remote", return_value=True), \
             mock.patch.object(mod, "detect_remote_usb_state", return_value={}), \
             mock.patch.object(mod, "print_usb_state_banner"), \
             mock.patch("builtins.input", return_value="FACTORY-RESET"), \
             mock.patch.object(sys, "argv", ["setup_remote.py", "--factory-reset"]):
            with self.assertRaises(SystemExit) as cm:
                mod.main()
        self.assertEqual(cm.exception.code, 0)
        self.assertEqual(len(sent), 1)
        # run the command it sends (printf of the script) and compare what it writes
        cmd = sent[0].split("; (sh ")[0].replace("/tmp/factory_reset.sh", str(self.base / "written.sh"))
        subprocess.run(["sh", "-c", cmd], check=True)
        self.assertEqual((self.base / "written.sh").read_bytes(), REMOTE_SCRIPT.read_bytes())
        self.assertIn("sh /tmp/factory_reset.sh --confirm", sent[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
