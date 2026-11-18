#!/usr/bin/env python3
"""Wi-Fi setup over USB: hub_util usbwifi (payload/source/usb_wifi.c, wifi_config.c).

hub_util is built for the host (TEST_HARNESS: the wpa_supplicant files, wpa_cli, the lock and
the state file live under a temporary root). The stock usbhid is replaced by the test: it
connects to the applet on a test port, sends the connection type byte 0x06, then passes 64-byte
frames, exactly as usbhid relays the USB reports. The PC side is tools/harmony_hid.py itself
(HarmonyHid with a transport that writes the frames to that socket), so provision_wifi() and
get_wifi_status() are tested unchanged against the applet. wpa_cli is a shell script: a
network whose password is "goodpassword" (or an open one named "OpenNet") connects, the
password "wrongpassword" fails the handshake, any other never connects; wpa_supplicant (a
script too) answers only once started when the file "no_supplicant" exists. Nothing on the
host's network is touched; the applet listens on 127.0.0.1 only.

Checks: status before a set (stock keys, no password); a set with a wrong password is
acknowledged, the status says failed with the reason, both configuration files keep their old
content; a good set saves both files and the status says connected; a timeout keeps the old
files; bad input (short password, WEP, unknown encryption, no SSID, an oversized request) is
refused without touching the files; a set while another Wi-Fi test holds the lock (a kernel
flock, also on an empty lock file, the moment a holder has just created it) is refused; a lock
file without a holder is taken; CRLF line ends are accepted; a handle id still open is never
handed out again after the one-byte ids wrap; every other file, flush, device control, commands and the
reset are refused with stock's error answer (byte 2 = 0xFF); malformed and unknown handles get
the parse and handle codes; ping answers; a new connection from usbhid replaces the old one and
is counted in the state file; the password never appears in the status or the log; the DHCP
client is asked for a new lease after a good set.

Previous settings really kept: with no Wi-Fi file before, a failed set removes the candidate
again (and drops it from wpa_supplicant); an applet killed in the middle of a test leaves its
copies and a mark on flash, and the next start puts the previous files back (also after a
power cut, which clears the lock in /tmp), with or without previous files. A Hub on which
wpa_supplicant does not run (no network stored) gets it started for the test. Only WPA2-PSK,
WPA-PSK and open networks are accepted (WPA-EAP and enterprise refused). A write announcing more
data frames than any request needs is refused before the count is used. Network names are
logged with control characters replaced. USB_WIFI_SETUP=0 in ethernet.conf refuses every
request.
"""

from __future__ import annotations

import fcntl
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SRC = REPO_DIR / "payload" / "source"
sys.path.insert(0, str(REPO_DIR))

from tools.harmony_hid import HarmonyHid, HidErrorResponse, build_primary_frames, encode_param_byte, encode_param_string  # noqa: E402

PORT = 9174
OLD_CONF = ('ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n\nnetwork={\n\tssid="OldNet"\n'
            '\tpsk="oldpassword1"\n}\n')

FAKE_WPA_CLI = r'''#!/bin/sh
# fake wpa_cli: the state follows the network in the configuration file
ROOT="@ROOT@"
CONF="$ROOT/etc/wpa_supplicant.conf"
echo "$*" >> "$ROOT/wpa_cli.log"
ssid=$(sed -n 's/^[[:space:]]*ssid="\(.*\)"$/\1/p' "$CONF" 2>/dev/null)
psk=$(sed -n 's/^[[:space:]]*psk="\(.*\)"$/\1/p' "$CONF" 2>/dev/null)
open=$(grep -c 'key_mgmt=NONE' "$CONF" 2>/dev/null)
case "$3" in
  ping) [ -f "$ROOT/no_supplicant" ] || echo PONG ;;
  reconfigure|remove_network|disconnect) echo OK ;;
  status)
    if [ -f "$ROOT/no_wpa" ]; then exit 1; fi
    if [ "$psk" = "goodpassword" ] || [ "$psk" = "oldpassword1" ] || { [ "$open" = 1 ] && [ "$ssid" = "OpenNet" ]; }; then
      echo "bssid=02:00:00:00:00:01"
      echo "ssid=$ssid"
      echo "key_mgmt=WPA2-PSK"
      echo "wpa_state=COMPLETED"
      echo "ip_address=192.0.2.10"
    elif [ "$psk" = "wrongpassword" ]; then
      echo "wpa_state=4WAY_HANDSHAKE"
      echo "auth_failures=1"
    elif [ "$psk" = "hubwrongkey" ]; then
      # what wpa_supplicant 0.7.2 on a Hub shows for a wrong key (2026-11-17): the handshake
      # starts and the state falls back to scanning, again and again; no WRONG_KEY, no counter
      n=$(cat "$ROOT/status_calls" 2>/dev/null || echo 0); n=$((n + 1)); echo "$n" > "$ROOT/status_calls"
      case $((n % 4)) in
        1) echo "wpa_state=SCANNING" ;;
        2|3) echo "bssid=02:00:00:00:00:01"; echo "wpa_state=4WAY_HANDSHAKE" ;;
        0) echo "wpa_state=SCANNING" ;;
      esac
    elif [ "$psk" = "neverdone12" ]; then
      echo "bssid=02:00:00:00:00:01"
      echo "wpa_state=ASSOCIATING"
    else
      echo "wpa_state=SCANNING"
    fi
    ;;
esac
'''


FAKE_SUPPLICANT = r'''#!/bin/sh
# fake wpa_supplicant: records how it was started and starts answering wpa_cli
ROOT="@ROOT@"
echo "$*" >> "$ROOT/supplicant.log"
rm -f "$ROOT/no_supplicant"
'''


def hub_util_sources() -> list[str]:
    """hub_util's sources as build/compile_all.sh lists them"""
    for line in (REPO_DIR / "build" / "compile_all.sh").read_text().splitlines():
        if '-o "$BIN/hub_util"' in line:
            return [str(REPO_DIR / t) for t in line.split() if t.startswith("payload/source/") and t.endswith(".c")]
    raise RuntimeError("hub_util build line not found in build/compile_all.sh")


class SocketTransport:
    """usbhid's side: 64-byte reports in and out over the applet's TCP connection"""

    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.sendall(b"\x06")  # connection type, as usbhid sends it
        self.buf = b""

    def write_report(self, data: bytes) -> bool:
        if len(data) == 65:
            data = data[1:]
        self.sock.sendall(bytes(data).ljust(64, b"\x00"))
        return True

    def read_report(self, timeout_ms: int = 5000) -> bytes:
        self.sock.settimeout(max(0.01, timeout_ms / 1000.0))
        try:
            while len(self.buf) < 64:
                chunk = self.sock.recv(256)
                if not chunk:
                    return b""
                self.buf += chunk
        except (socket.timeout, BlockingIOError):
            return b""
        rep, self.buf = self.buf[:64], self.buf[64:]
        return rep

    def close(self) -> None:
        self.sock.close()


class UsbWifi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.bin = pathlib.Path(cls.tmp.name) / "hub_util"
        subprocess.run(["gcc", "-Wall", "-O2", "-DTEST_HARNESS", f"-I{SRC}", "-o", str(cls.bin),
                        *hub_util_sources(), "-lm"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.rootdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.rootdir.name)
        for d in ("etc", "tmp", "usr/sbin", "var/run"):
            (self.root / d).mkdir(parents=True)
        (self.root / "etc" / "wpa_supplicant.conf").write_text(OLD_CONF)
        (self.root / "etc" / "wpa_supplicant.ath0.conf").write_text(OLD_CONF)
        cli = self.root / "usr" / "sbin" / "wpa_cli"
        cli.write_text(FAKE_WPA_CLI.replace("@ROOT@", str(self.root)))
        cli.chmod(0o755)
        sup = self.root / "usr" / "sbin" / "wpa_supplicant"
        sup.write_text(FAKE_SUPPLICANT.replace("@ROOT@", str(self.root)))
        sup.chmod(0o755)
        self.env = dict(os.environ, HARMONY_WIFI_ROOT=str(self.root), HARMONY_WIFI_TEST_WAIT="2")
        self.log = open(self.root / "applet.log", "w+")
        self.transports = []
        self.dhcp = None
        self.start_applet()

    def start_applet(self, wait: str = "2") -> None:
        self.env["HARMONY_WIFI_TEST_WAIT"] = wait
        self.proc = subprocess.Popen([str(self.bin), "usbwifi", "--port", str(PORT), "--verbose"],
                                     env=self.env, stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)

    def tearDown(self):
        for t in self.transports:
            t.close()
        self.proc.kill()
        self.proc.wait()
        if self.dhcp:
            self.dhcp.kill()
            self.dhcp.wait()
        self.log.close()
        self.rootdir.cleanup()

    def hid(self) -> HarmonyHid:
        t = SocketTransport(PORT)
        self.transports.append(t)
        h = HarmonyHid()
        h.dev = t
        return h

    def confs(self) -> tuple[str, str]:
        return ((self.root / "etc" / "wpa_supplicant.conf").read_text(),
                (self.root / "etc" / "wpa_supplicant.ath0.conf").read_text())

    def state(self) -> dict[str, str]:
        p = self.root / "tmp" / "harmony_usbwifi.state"
        return dict(line.split("=", 1) for line in p.read_text().splitlines() if "=" in line)

    def applet_log(self) -> str:
        self.log.flush()
        return (self.root / "applet.log").read_text()

    # ---- the two PC functions, unchanged ----

    def test_status_before_any_set(self):
        st = self.hid().get_wifi_status()
        self.assertEqual(st.get("connect_status"), "connected")
        self.assertEqual(st.get("ssid"), "OldNet")
        self.assertEqual(st.get("ip_address"), "192.0.2.10")
        self.assertEqual(st.get("password"), "")
        self.assertEqual(st.get("error_code"), "none")
        self.assertEqual(st.get("firmware"), "harmony-ng")
        self.assertEqual(st.get("last_attempt"), "none")

    def test_good_set_saves_both_files(self):
        # a DHCP client on ath0 that records the renew signal
        mark = self.root / "renewed"
        self.dhcp = subprocess.Popen(["sh", "-c", f"trap 'echo r > {mark}' USR1; while :; do sleep 0.05; done"])
        (self.root / "var" / "run" / "udhcpc.ath0.pid").write_text(f"{self.dhcp.pid}\n")
        time.sleep(0.2)
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("New Net,2", "goodpassword"))
        conf, ath0 = self.confs()
        self.assertEqual(conf, ath0, "both files hold the same configuration")
        self.assertIn('ssid="New Net,2"\n', conf)
        self.assertIn('psk="goodpassword"\n', conf)
        self.assertIn("scan_ssid=1", conf)
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "connected")
        self.assertEqual(st.get("ssid"), "New Net,2", "an SSID with a comma comes back whole")
        self.assertEqual(st.get("last_attempt"), "ok")
        self.assertEqual(self.state().get("last_attempt"), "ok")
        for _ in range(40):
            if mark.exists():
                break
            time.sleep(0.05)
        self.assertTrue(mark.exists(), "the DHCP client on ath0 was asked for a new lease")
        self.assertNotIn("goodpassword", self.applet_log())

    def test_open_network(self):
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("OpenNet", "", "NONE"))
        conf, _ = self.confs()
        self.assertIn("key_mgmt=NONE", conf)
        self.assertNotIn("psk=", conf)
        self.assertEqual(hid.get_wifi_status().get("connect_status"), "connected")

    def test_wrong_password_keeps_old_config(self):
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("NewNet", "wrongpassword"), "the write is acknowledged")
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF), "the previous configuration is kept")
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "failed")
        self.assertIn("Incorrect Wi-Fi password", st.get("error_code", ""))
        self.assertEqual(st.get("password"), "")
        self.assertNotIn("wrongpassword", "".join(st.values()))
        self.assertNotIn("wrongpassword", self.applet_log())
        self.assertEqual(self.state().get("last_attempt"), "failed")
        # wpa_supplicant was pointed back at the old network
        self.assertGreaterEqual((self.root / "wpa_cli.log").read_text().count("reconfigure"), 2)

    def test_network_not_found_keeps_old_config(self):
        hid = self.hid()
        t0 = time.time()
        self.assertTrue(hid.provision_wifi("FarAway", "unreachable1"))
        self.assertLess(time.time() - t0, 15)
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF))
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "failed")
        self.assertIn("network not found", st.get("error_code", ""))

    def test_timeout_keeps_old_config(self):
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("SlowNet", "neverdone12"))
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF))
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "failed")
        self.assertIn("timed out", st.get("error_code", ""))

    def test_wrong_key_seen_as_handshake_drops(self):
        """The Hub's wpa_supplicant never says WRONG_KEY: two handshakes that fall back to
        scanning are a wrong password, reported before the test wait runs out"""
        self.proc.kill()
        self.proc.wait()
        self.start_applet(wait="20")
        hid = self.hid()
        hid.dev.sock.settimeout(60)
        t0 = time.time()
        self.assertTrue(hid.provision_wifi("NewNet", "hubwrongkey"))
        self.assertLess(time.time() - t0, 15, "ended at the second drop, not at the 20 s wait")
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF))
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "failed")
        self.assertIn("Incorrect Wi-Fi password", st.get("error_code", ""))
        self.assertNotIn("hubwrongkey", self.applet_log())

    def test_bad_input_refused_without_writing(self):
        cases = [
            (("NewNet", "short", "WPA2-PSK"), "8 to 63"),
            (("NewNet", "0123456789", "WEP"), "WEP"),
            (("NewNet", "goodpassword", "TKIP-ENTERPRISE"), "Unsupported encryption"),
            (("NewNet", "goodpassword", "WPA-EAP"), "Unsupported encryption"),
            (("NewNet", "goodpassword", "WPA2-ENTERPRISE"), "enterprise networks are not supported"),
            (("", "goodpassword", "WPA2-PSK"), "SSID missing"),
            (("x" * 33, "goodpassword", "WPA2-PSK"), "32 bytes"),
            (("NewNet", "good\tpassword", "WPA2-PSK"), "control character"),
        ]
        hid = self.hid()
        for (ssid, pw, enc), reason in cases:
            with self.subTest(ssid=ssid[:8], enc=enc):
                self.assertTrue(hid.provision_wifi(ssid, pw, enc))
                st = hid.get_wifi_status()
                self.assertEqual(st.get("connect_status"), "failed")
                self.assertIn(reason, st.get("error_code", ""))
                self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF))
        self.assertNotIn("reconfigure", (self.root / "wpa_cli.log").read_text())

    def test_oversized_request_refused(self):
        hid = self.hid()
        handle = hid.open_file_write("/sys/wifi/connect", size=1500)
        self.assertTrue(hid.write_file_chunks(handle, b"ssid,Big\npassword," + b"p" * 1500 + b"\n",
                                              packets_per_chunk=40, count_width="byte"))
        self.assertTrue(hid.close_file(handle))
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "failed")
        self.assertIn("too long", st.get("error_code", ""))
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF))

    def lock_path(self) -> pathlib.Path:
        return self.root / "tmp" / "harmony_wifi_test.lock"

    def hold_lock(self):
        """Another Wi-Fi test in progress: holds the kernel lock until the file is closed"""
        fh = open(self.lock_path(), "a+")
        fcntl.flock(fh, fcntl.LOCK_EX)
        self.addCleanup(fh.close)
        return fh

    def lock_free(self) -> bool:
        with open(self.lock_path(), "a+") as fh:
            try:
                fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return False
            fcntl.flock(fh, fcntl.LOCK_UN)
            return True

    def test_busy_while_another_test_runs(self):
        self.lock_path().write_text("")  # just created by the other test, nothing written in it
        self.hold_lock()
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("NewNet", "goodpassword"))
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "failed")
        self.assertIn("Another Wi-Fi test", st.get("error_code", ""))
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF))
        self.assertTrue(self.lock_path().exists(), "another holder's lock file is left alone")
        self.assertFalse(self.lock_free(), "another holder's lock is left alone")

    def test_lock_file_without_holder_taken(self):
        dead = subprocess.Popen(["true"])
        dead.wait()
        self.lock_path().write_text(f"{dead.pid}\n")  # left by an older build, nobody holds it
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("NewNet", "goodpassword"))
        self.assertEqual(hid.get_wifi_status().get("connect_status"), "connected")
        self.assertTrue(self.lock_free(), "the lock is released after the set")

    def test_crlf_line_ends(self):
        hid = self.hid()
        payload = b"ssid,CrlfNet\r\npassword,goodpassword\r\nencryption,WPA2-PSK\r\n"
        handle = hid.open_file_write("/sys/wifi/connect", size=len(payload))
        self.assertTrue(hid.write_file_chunks(handle, payload, packets_per_chunk=50, count_width="byte"))
        self.assertTrue(hid.close_file(handle))
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "connected", st)
        self.assertEqual(st.get("ssid"), "CrlfNet")
        conf, _ = self.confs()
        self.assertIn('ssid="CrlfNet"\n', conf)
        self.assertIn('psk="goodpassword"\n', conf)

    def test_handle_ids_wrap_past_open_handles(self):
        hid = self.hid()
        held = hid.open_file_write("/sys/wifi/connect", size=10)  # stays open
        ids = []
        for _ in range(300):  # the one-byte ids wrap past the held one
            h = hid.open_file_write("/sys/wifi/connect", size=10)
            ids.append(h)
            self.assertTrue(hid.close_file(h))
        self.assertNotIn(held, ids, "an id still open was handed out again")
        self.assertNotIn(0, ids)
        self.assertTrue(hid.close_file(held))

    # ---- the previous settings, really kept ----

    def paths(self) -> tuple[pathlib.Path, pathlib.Path]:
        return self.root / "etc" / "wpa_supplicant.conf", self.root / "etc" / "wpa_supplicant.ath0.conf"

    def leftovers(self) -> list[str]:
        return sorted(p.name for p in (self.root / "etc").iterdir() if "pretest" in p.name)

    def test_no_previous_config_failed_set_removes_candidate(self):
        for p in self.paths():
            p.unlink()
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("NewNet", "wrongpassword"))
        self.assertEqual(hid.get_wifi_status().get("connect_status"), "failed")
        for p in self.paths():
            self.assertFalse(p.exists(), f"{p.name}: the failed candidate (and its password) stayed")
        self.assertEqual(self.leftovers(), [])
        self.assertIn("remove_network all", (self.root / "wpa_cli.log").read_text(),
                      "the candidate was not dropped from the running wpa_supplicant")

    def test_only_the_ath0_file_before(self):
        conf, ath0 = self.paths()
        conf.unlink()
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("FarAway", "unreachable1"))
        self.assertFalse(conf.exists(), "a file that did not exist was created")
        self.assertEqual(ath0.read_text(), OLD_CONF)

    def test_supplicant_not_running_is_started(self):
        for p in self.paths():
            p.unlink()
        (self.root / "no_supplicant").write_text("")
        hid = self.hid()
        self.assertTrue(hid.provision_wifi("NewNet", "goodpassword"))
        st = hid.get_wifi_status()
        self.assertEqual(st.get("connect_status"), "connected", st)
        started = (self.root / "supplicant.log").read_text()
        self.assertIn(f"-s -iath0 -c {self.root}/etc/wpa_supplicant.conf", started)
        conf, ath0 = self.confs()
        self.assertIn('ssid="NewNet"', conf)
        self.assertEqual(conf, ath0)
        self.assertEqual(self.leftovers(), [])

    def interrupt_test(self) -> None:
        """An applet with a long test wait is killed while its test runs"""
        self.proc.kill()
        self.proc.wait()
        self.start_applet(wait="60")
        hid = self.hid()
        hid.dev.sock.settimeout(120)
        payload = b"ssid,FarAway\npassword,unreachable1\nencryption,WPA2-PSK\n"
        handle = hid.open_file_write("/sys/wifi/connect", size=len(payload))
        for f in build_primary_frames(0x03, 9, [encode_param_byte(handle), encode_param_byte(2)]):
            hid.dev.write_report(f)
        hid.dev.write_report(bytes([0x00, len(payload)]) + payload)
        conf, _ = self.paths()
        for _ in range(100):
            if conf.exists() and "FarAway" in conf.read_text():
                break
            time.sleep(0.05)
        self.assertIn("FarAway", conf.read_text(), "the test did not start")
        self.proc.kill()
        self.proc.wait()

    def test_killed_mid_test_restored_at_start(self):
        self.interrupt_test()
        self.assertIn("FarAway", self.paths()[0].read_text())
        self.assertIn("wpa_supplicant.pretest", self.leftovers(), "no mark left on flash")
        self.assertTrue(self.lock_free(), "the kernel dropped the killed applet's lock")
        self.start_applet()
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF), "the previous files are back")
        self.assertEqual(self.leftovers(), [])
        self.assertTrue(self.lock_free())
        self.assertIn("interrupted Wi-Fi test was undone", self.applet_log())
        self.assertTrue((self.root / "wpa_cli.log").read_text().rstrip().endswith("reconfigure"))

    def test_power_cut_mid_test_without_previous_files(self):
        for p in self.paths():
            p.unlink()
        self.interrupt_test()
        (self.root / "tmp" / "harmony_wifi_test.lock").unlink()  # /tmp is RAM: gone after a power cut
        self.start_applet()
        for p in self.paths():
            self.assertFalse(p.exists(), f"{p.name}: the candidate of the cut test stayed")
        self.assertEqual(self.leftovers(), [])

    def test_recovery_waits_for_a_live_test(self):
        mark = self.root / "etc" / "wpa_supplicant.pretest"
        mark.write_text("conf=0\nath0=0\n")
        self.hold_lock()  # a live test runs
        self.proc.kill()
        self.proc.wait()
        self.start_applet()
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF), "the live test's files were touched")
        self.assertTrue(mark.exists())

    # ---- input checks, settings ----

    def test_huge_frame_count_refused(self):
        hid = self.hid()
        handle = hid.open_file_write("/sys/wifi/connect", size=10)
        for count in (0x80000000, 0xFFFFFFFF, 1025):
            frames = build_primary_frames(0x03, 7, [encode_param_byte(handle), bytes([0x04]) + count.to_bytes(4, "big")])
            hid.dev.write_report(frames[0])
            resp = hid.dev.read_report(3000)
            self.assertEqual(resp[:6], bytes([0xFF, 0x03, 0xFF, 0x01, 0x01, 0x0C]), hex(count))
        self.assertEqual(hid.get_wifi_status().get("ssid"), "OldNet", "still serving")

    def test_ssid_logged_without_control_characters(self):
        hid = self.hid()
        hid.provision_wifi("Net\x1b[2Jx\x07", "goodpassword", "WPA-EAP")
        log = self.applet_log()
        self.assertNotIn("\x1b", log)
        self.assertNotIn("\x07", log)
        self.assertIn("Net?[2Jx?", log)

    def test_turned_off_in_settings(self):
        (self.root / "data" / "harmony").mkdir(parents=True)
        (self.root / "data" / "harmony" / "ethernet.conf").write_text('ETH_ENABLED=1\nUSB_WIFI_SETUP="0"\n')
        hid = self.hid()
        self.assertEqual(hid.get_wifi_status(), {}, "the status is readable with the setup turned off")
        with self.assertRaises(HidErrorResponse) as cm:
            hid.provision_wifi("NewNet", "goodpassword")
        self.assertEqual(cm.exception.code, 0x0B)
        self.assertEqual(self.confs(), (OLD_CONF, OLD_CONF))
        self.assertIn("turned off in the settings", self.applet_log())
        (self.root / "data" / "harmony" / "ethernet.conf").write_text("ETH_ENABLED=1\nUSB_WIFI_SETUP=1\n")
        self.assertEqual(hid.get_wifi_status().get("ssid"), "OldNet", "on again without a restart")

    # ---- everything else is refused ----

    def raw(self, hid: HarmonyHid, cmd: int, params: list[bytes]) -> bytes:
        return hid.command(cmd, params, timeout_ms=3000)

    def test_other_files_and_commands_refused(self):
        hid = self.hid()
        for path in ("/sys/dropbear", "/tde/enable", "/sys/reboot", "/sys/factoryreset", "/sys/sysinfo",
                     "/sys/wifi/connectX", "/sys/wifi", "/fw/otaupdate"):
            with self.subTest(path=path):
                with self.assertRaises(HidErrorResponse) as cm:
                    hid.open_file_write(path, size=4)
                self.assertEqual(cm.exception.code, 0x0B)
        self.assertFalse(hid.set_tde_enable(True))
        self.assertEqual(hid.get_sysinfo(), {})
        with self.assertRaises(HidErrorResponse):
            hid.write_full_file("/sys/dropbear", b"x" * 100)
        for cmd in (0x05, 0x06, 0x08, 0x09, 0x02, 0x42):
            resp = self.raw(hid, cmd, [encode_param_byte(1)])
            self.assertEqual(resp[:6], bytes([0xFF, cmd, 0xFF, 0x01, 0x01, 0x0B]), hex(cmd))
        resp = self.raw(hid, 0xFF, [encode_param_byte(0x66)])
        self.assertEqual(resp[:6], bytes([0xFF, 0xFF, 0xFF, 0x01, 0x01, 0x0B]), "the firmware reset is refused")
        self.assertFalse(hid.reset_filesystem())

    def test_ping_parse_and_handle_errors(self):
        hid = self.hid()
        resp = self.raw(hid, 0x00, [])
        self.assertEqual(resp[:4], bytes([0xFF, 0x00, (hid.seq - 1) | 0x80, 0x00]))
        # a string without its NUL byte runs past the frame
        bad = bytearray(build_primary_frames(0x01, 5, [encode_param_string("/sys/wifi/connect")])[0])
        bad[4] = 0x80
        for i in range(5, 64):
            bad[i] = 0x41
        hid.dev.write_report(bytes(bad))
        resp = hid.dev.read_report(3000)
        self.assertEqual(resp[:6], bytes([0xFF, 0x01, 0xFF, 0x01, 0x01, 0x0C]))
        # read and close of a handle that was never opened
        self.assertEqual(self.raw(hid, 0x04, [encode_param_byte(9), encode_param_byte(1)])[:6],
                         bytes([0xFF, 0x04, 0xFF, 0x01, 0x01, 0x04]))
        self.assertFalse(hid.close_file(9))
        # an open without a mode letter
        resp = self.raw(hid, 0x01, [encode_param_string("/sys/wifi/connect"), encode_param_string("X")])
        self.assertEqual(resp[:6], bytes([0xFF, 0x01, 0xFF, 0x01, 0x01, 0x0C]))
        # still serving after all of it
        self.assertEqual(hid.get_wifi_status().get("ssid"), "OldNet")

    def test_new_connection_replaces_old(self):
        first = self.hid()
        self.assertEqual(first.get_wifi_status().get("ssid"), "OldNet")
        before = int(self.state().get("connections", "0"))
        second = self.hid()  # usbhid connects again when a computer configures the device
        self.assertEqual(second.get_wifi_status().get("ssid"), "OldNet")
        self.assertEqual(int(self.state().get("connections", "0")), before + 1)
        first.dev.sock.settimeout(2)
        self.assertEqual(first.dev.sock.recv(64), b"", "the old connection is closed")

    def test_status_command_line(self):
        res = subprocess.run([str(self.bin), "usbwifi", "--status"], env=self.env, capture_output=True,
                             text=True, timeout=20)
        self.assertEqual(res.returncode, 0)
        self.assertIn("connect_status,connected\n", res.stdout)
        self.assertIn("password,\n", res.stdout)

    def test_listens_on_loopback_only(self):
        listen = [l for l in pathlib.Path("/proc/net/tcp").read_text().splitlines()[1:]
                  if l.split()[1].endswith(":%04X" % PORT) and l.split()[3] == "0A"]
        self.assertEqual(len(listen), 1)
        self.assertTrue(listen[0].split()[1].startswith("0100007F:"), listen[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
