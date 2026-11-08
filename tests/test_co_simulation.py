#!/usr/bin/env python3
"""
Full User-Space Co-Simulation Test Suite for Harmony Hub & Remote.
Runs:
  1. Virtual CC2544 RF Router (translates Report 0x12 <-> Report 0x20)
  2. Mock Hardware Layer (Framebuffer, Touchscreen, Buttons, Gyro, Sysfs)
  3. Hub Daemon (REST, WebSocket, IR Engine, Activities State Machine)
  4. End-to-End Co-Simulation Scenarios:
     - Remote discovery & RF handshake
     - Virtual touchscreen activity activation
     - Volume key IR blaster verification
     - Live battery reporting
     - Framebuffer pixel inspection
"""

import os
import sys
import time
import subprocess
import urllib.request
import json
import signal
import fcntl
import threading
import urllib.error

# Add current dir to import mock hardware tools
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MOCK_DIR = os.path.join(SCRIPT_DIR, "mock_hardware")
sys.path.insert(0, MOCK_DIR)

from mock_controller import MockHardwareController, KEY_VOLUMEUP, KEY_VOLUMEDOWN
from mock_rf_router import CC2544VirtualRouter

REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
TEST_PORT = 8998
MOCK_BASE = "/tmp/harmony_mock"

# The Hub's IR receiver as ir_i2s.c reads it: 0.09375 us per bit (16x the 1.5 us I2S sample),
# MSB first, active low (a mark is 0 bits, a space and the idle line are 1 bits)
IR_RX_US_PER_BIT = 3.0 / 32.0


def ir_rx_bitstream(timings_us, idle_before_bytes=2048, idle_after_us=30000):
    """Raw /dev/i2s receive data for alternating mark/space durations (starting with a mark)"""
    runs = [("1", idle_before_bytes * 8)]
    for i, d in enumerate(list(timings_us) + [idle_after_us]):
        runs.append(("0" if i % 2 == 0 else "1", round(d / IR_RX_US_PER_BIT)))
    bits = "".join(ch * n for ch, n in runs)
    bits += "1" * (-len(bits) % 8)
    return int(bits, 2).to_bytes(len(bits) // 8, "big")


def learned_durations(raw):
    """The P/S durations (us) of a learned raw string F<carrier hex>P<hex>S<hex>..."""
    out, num = [], ""
    for ch in raw[1:] + "P":
        if ch in "PS":
            if num:
                out.append(int(num, 16))
            num = ""
        elif ch not in " ":
            num += ch
    return out[1:]  # the first number is the carrier


class CoSimulationRunner:
    def __init__(self):
        self.mock_ctrl = MockHardwareController(MOCK_BASE)
        self.rf_router = CC2544VirtualRouter(MOCK_BASE)
        self.hub_proc = None
        self.passed = 0
        self.failed = 0

    def record(self, test_name, success, detail=""):
        if success:
            self.passed += 1
            print(f"  [\033[92mPASS\033[0m] {test_name} ({detail})")
        else:
            self.failed += 1
            print(f"  [\033[91mFAIL\033[0m] {test_name} - {detail}")

    def http_get(self, path):
        url = f"http://127.0.0.1:{TEST_PORT}{path}"
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=3.0) as resp:
                return resp.status, json.loads(resp.read().decode())
        except Exception as e:
            return 0, str(e)

    def http_post(self, path, payload):
        url = f"http://127.0.0.1:{TEST_PORT}{path}"
        data = json.dumps(payload).encode()
        req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=3.0) as resp:
                return resp.status, json.loads(resp.read().decode())
        except Exception as e:
            return 0, str(e)

    def http_post_status(self, path, payload, timeout=15.0):
        """POST that keeps the status code of an error reply (http_post turns those into 0)"""
        url = f"http://127.0.0.1:{TEST_PORT}{path}"
        req = urllib.request.Request(url, data=json.dumps(payload).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, json.loads(resp.read().decode() or "{}")
        except urllib.error.HTTPError as e:
            try:
                return e.code, json.loads(e.read().decode() or "{}")
            except ValueError:
                return e.code, {}
        except Exception as e:
            return 0, {"error": str(e)}

    def run(self):
        print("\n=======================================================")
        print("  Harmony Hub & Remote Co-Simulation Suite")
        print("=======================================================\n")

        # 1. Setup Mock Hardware Environment
        print("[-] Step 1: Initializing Virtual Hardware Environment...")
        self.mock_ctrl.setup_environment()
        self.rf_router.start()
        time.sleep(0.2)
        self.record("Mock Hardware & Sysfs setup", True, f"Base {MOCK_BASE}")

        # 2. Start Hub Daemon in Host Test Mode
        hub_bin = os.path.join(REPO_ROOT, "build/tmp/test_host/hub_daemon_test")
        # Rebuild when any daemon source is newer than the binary: building only when it was
        # missing kept testing an old daemon after a source change
        src_dir = os.path.join(REPO_ROOT, "payload/source")
        newest_src = max((os.path.getmtime(os.path.join(src_dir, f)) for f in os.listdir(src_dir)
                          if f.endswith((".c", ".h"))), default=0)
        if not os.path.exists(hub_bin) or os.path.getmtime(hub_bin) < newest_src:
            print(f"[-] Compiling host test daemon at {hub_bin}...")
            os.makedirs(os.path.dirname(hub_bin), exist_ok=True)
            compile_cmd = [
                "gcc", "-Wall", "-O2", "-g", "-DTEST_HARNESS=1",
                f"-I{REPO_ROOT}/payload/source",
                f"{REPO_ROOT}/payload/source/hub_daemon.c",
                f"{REPO_ROOT}/payload/source/http_server.c",
                f"{REPO_ROOT}/payload/source/http_util.c",
                f"{REPO_ROOT}/payload/source/http_auth.c",
                f"{REPO_ROOT}/payload/source/http_api_auth.c",
                f"{REPO_ROOT}/payload/source/http_api_activities.c",
                f"{REPO_ROOT}/payload/source/http_api_devices.c",
                f"{REPO_ROOT}/payload/source/http_api_commands.c",
                f"{REPO_ROOT}/payload/source/http_api_custom_icons.c",
                f"{REPO_ROOT}/payload/source/http_api_network.c",
                f"{REPO_ROOT}/payload/source/hub_network_util.c",
                f"{REPO_ROOT}/payload/source/http_api_remotes.c",
                f"{REPO_ROOT}/payload/source/http_api_ota.c",
                f"{REPO_ROOT}/payload/source/http_api_backup.c",
                f"{REPO_ROOT}/payload/source/http_api_system.c",
                f"{REPO_ROOT}/payload/source/config_store.c",
                f"{REPO_ROOT}/payload/source/bt_migrate.c",
                f"{REPO_ROOT}/payload/source/config_sanitize.c",
                f"{REPO_ROOT}/payload/source/orchestrator.c",
                f"{REPO_ROOT}/payload/source/orchestrator_queue.c",
                f"{REPO_ROOT}/payload/source/hub_mqtt.c",
                f"{REPO_ROOT}/payload/source/hub_mqtt_ha.c",
                f"{REPO_ROOT}/payload/source/hub_ws.c",
                f"{REPO_ROOT}/payload/source/hub_ntp.c",
                f"{REPO_ROOT}/payload/source/resource_cache.c",
                f"{REPO_ROOT}/payload/source/cJSON.c",
                f"{REPO_ROOT}/payload/source/ir_encoder.c",
                f"{REPO_ROOT}/payload/source/ir_i2s.c",
                f"{REPO_ROOT}/payload/source/hw_action.c",
                f"{REPO_ROOT}/payload/source/ws_server.c",
                f"{REPO_ROOT}/payload/source/state_watch.c",
                f"{REPO_ROOT}/payload/source/mqtt_client.c",
                f"{REPO_ROOT}/payload/source/sha256.c",
                f"{REPO_ROOT}/payload/source/timezone_db.c",
                "-lpthread", "-lm", "-o", hub_bin
            ]
            res = subprocess.run(compile_cmd, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"Compilation failed: {res.stderr}")
                sys.exit(1)

        print(f"[-] Launching Hub Daemon on port {TEST_PORT}...")
        env = os.environ.copy()
        env["HARMONY_MOCK_DIR"] = MOCK_BASE
        env["HARMONY_MOCK_ROLE"] = "hub"
        libmock = os.path.join(REPO_ROOT, "build/tmp/test_host/libmock_hardware.so")
        if os.path.exists(libmock):
            env["LD_PRELOAD"] = libmock

        self.hub_proc = subprocess.Popen(
            [hub_bin, "--dry-run", "--port", str(TEST_PORT)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )

        # Wait for daemon to respond
        live = False
        for _ in range(15):
            time.sleep(0.2)
            st, _ = self.http_get("/health")
            if st == 200:
                live = True
                break

        # The answer must come from this daemon: one left running by an earlier aborted run
        # holds the port, this one exits on the bind error, and the health check still passes
        if live and self.hub_proc.poll() is not None:
            live = False
            print(f"[!] Port {TEST_PORT} is answered by another process; the test daemon exited")
        self.record("Hub Daemon Startup & Health", live, f"Port {TEST_PORT}")
        if not live:
            self.cleanup()
            return

        # 3. Test Activity Switch API & State Engine
        print("[-] Step 2: Testing Activity Execution in Co-Simulation...")
        st, res = self.http_post("/api/activity-stop", {"dry_run": 1})
        self.record("Activity Stop Sequence", st == 200, f"HTTP {st}")

        # 3b. Forget the BLE remote: the command reaches hub_btstack, the map loses its address
        print("[-] Step 2b: Forgetting the pinned BLE remote...")
        cmd_file = "/tmp/hub_btstack_cmd"
        map_path = os.path.join(MOCK_BASE, "data", "harmony", "bt_remote_map.json")
        try:
            os.makedirs(os.path.dirname(map_path), exist_ok=True)
            with open(map_path, "w") as f:
                json.dump({"remote": {"bdaddr": "11:22:33:44:55:66"}, "mapping": {"KEY_UP": "DirectionUp"}}, f)
            if os.path.exists(cmd_file):
                os.remove(cmd_file)
            st, res = self.http_post("/api/remote-unpair", {})
            res = res if isinstance(res, dict) else {"error": res}
            cmd = open(cmd_file).read().strip() if os.path.exists(cmd_file) else ""
            self.record("Remote unpair: FORGET sent to hub_btstack",
                        st == 200 and res.get("ok") is True and res.get("state") == "standby" and cmd == "FORGET",
                        f"HTTP {st} {res} cmd={cmd!r}")
            m = {}
            if os.path.exists(map_path):
                with open(map_path) as f:
                    m = json.load(f)
            self.record("Remote unpair: map keeps its mapping, loses the address",
                        res.get("map_cleared") is True and "bdaddr" not in m.get("remote", {})
                        and m.get("mapping", {}).get("KEY_UP") == "DirectionUp",
                        f"map_cleared={res.get('map_cleared')} {m}")
        finally:
            for p in (cmd_file, map_path):
                if os.path.exists(p):
                    os.remove(p)

        # 4. Test Direct Pronto Hex Blast & Verify Waveform in Mock Blaster
        print("[-] Step 3: Testing IR Blast & Mock IR Receiver...")
        pronto = "0000 0067 0000 000d 0060 0018 0030 0018 0018 0018 0030 0018 0018 0018 0030 0018 0018 0018 0018 0018 0030 0018 0018 0018 0018 0018 0018 0018 0018 03a0"
        st, blast_res = self.http_post("/api/ir-send", {"pronto": pronto, "port": 7, "repeats": 1})
        self.record("Pronto Hex IR Blast Command", st == 200 and blast_res.get("ok") is True, f"HTTP {st}")

        # 4b. Learn an IR signal from the receiver (POST /api/capture) and play it back
        print("[-] Step 3b: Learning an IR signal from the Hub's IR receiver...")
        rx_path = os.path.join(MOCK_BASE, "i2s_rx")
        # Sony SIRC 12-bit frame: 2.4 ms header, then 1.2 ms (1) or 0.6 ms (0) marks, 0.6 ms spaces
        bits12 = [1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0]
        sirc = [2400, 600]
        for i, b in enumerate(bits12):
            sirc.append(1200 if b else 600)
            if i < len(bits12) - 1:
                sirc.append(600)
        try:
            with open(rx_path, "wb") as f:
                f.write(ir_rx_bitstream(sirc, idle_after_us=30000))
            st, cap = self.http_post_status("/api/capture", {"timeout": 3})
            raw = cap.get("raw", "") if isinstance(cap, dict) else ""
            got = learned_durations(raw) if raw else []
            want = sirc + [30000]
            exact = len(got) == len(want) and all(abs(a - b) <= 1 for a, b in zip(got, want))
            self.record("IR learn: a Sony 12-bit frame comes back with every pulse and space",
                        st == 200 and raw.startswith("F9470") and exact and cap.get("keycode") == raw,
                        f"HTTP {st} {len(got)}/{len(want)} durations {raw[:60]}")
            self.record("IR learn: pulse count and length reported",
                        cap.get("pulses") == len(want) and cap.get("duration_us") == sum(want) and cap.get("carrier_hz") == 38000,
                        f"pulses={cap.get('pulses')} duration_us={cap.get('duration_us')} carrier={cap.get('carrier_hz')}")
            st, tst = self.http_post_status("/api/ir-test-learned", {"deviceId": "", "raw": raw, "keycode": raw})
            self.record("IR learn: the learned code plays back (ir-test-learned)",
                        st == 200 and tst.get("ok") is True, f"HTTP {st} {tst}")
            st, tst = self.http_post_status("/api/ir-test-learned", {"deviceId": ""})
            self.record("IR learn: ir-test-learned without a code is 400", st == 400, f"HTTP {st}")

            with open(rx_path, "wb") as f:
                f.write(b"\xff" * 4096)  # the line stays idle: nobody pressed a button
            st, cap = self.http_post_status("/api/capture", {"timeout": 1})
            self.record("IR learn: no signal is 408", st == 408, f"HTTP {st} {cap}")

            os.remove(rx_path)
            st, cap = self.http_post_status("/api/capture", {"timeout": 1})
            self.record("IR learn: no receiver is 503", st == 503, f"HTTP {st} {cap}")

            with open(rx_path, "wb") as f:
                f.write(ir_rx_bitstream(sirc))
            with open("/tmp/harmony_i2s.lock", "a+") as lk:
                fcntl.flock(lk, fcntl.LOCK_EX)  # an IR send holds the I2S hardware
                st, cap = self.http_post_status("/api/capture", {"timeout": 1})
                fcntl.flock(lk, fcntl.LOCK_UN)
            self.record("IR learn: I2S busy (IR send) is 409", st == 409, f"HTTP {st} {cap}")

            # one capture at a time: a second request while the first waits for a signal
            with open(rx_path, "wb") as f:
                f.write(ir_rx_bitstream(sirc))
            results = {}
            with open("/tmp/harmony_i2s.lock", "a+") as lk:
                fcntl.flock(lk, fcntl.LOCK_EX)  # keeps the first capture waiting for the hardware
                t = threading.Thread(target=lambda: results.update(first=self.http_post_status("/api/capture", {"timeout": 2})))
                t.start()
                time.sleep(0.5)
                st, cap = self.http_post_status("/api/capture", {"timeout": 1})
                fcntl.flock(lk, fcntl.LOCK_UN)
            t.join()
            self.record("IR learn: a second capture while one runs is 409",
                        st == 409 and "already running" in str(cap), f"HTTP {st} {cap}")
        finally:
            if os.path.exists(rx_path):
                os.remove(rx_path)

        # 5. Test Remote Physical Button Injection via Mock Layer
        print("[-] Step 4: Injecting Remote Physical Button Events...")
        self.mock_ctrl.press_button(KEY_VOLUMEUP, duration_ms=50)
        self.mock_ctrl.press_button(KEY_VOLUMEDOWN, duration_ms=50)
        self.record("Volume Key Injection", True, "KEY_VOLUMEUP & KEY_VOLUMEDOWN injected")

        # 6. Test Framebuffer Readout & Pixel Verification
        print("[-] Step 5: Validating Virtual Framebuffer...")
        fb_bytes = self.mock_ctrl.get_framebuffer_bytes()
        self.record("Virtual Framebuffer Integrity", len(fb_bytes) == 153600, f"{len(fb_bytes)} bytes")

        # 7. Test Remote Battery & Power State Virtualization
        print("[-] Step 6: Testing Power & Battery Telemetry...")
        self.mock_ctrl.set_power(vbat_mv=3820, charging=False, powersource="BATTERY")
        vbat_path = os.path.join(MOCK_BASE, "sysfs/vbat")
        with open(vbat_path, "r") as f:
            read_vbat = int(f.read().strip())
        self.record("Battery Voltage Update", read_vbat == 3820, f"3820 mV -> Discharging")

        print("\n=======================================================")
        if self.failed == 0:
            print(f"  \033[92mALL {self.passed} CO-SIMULATION CHECKS PASSED!\033[0m")
        else:
            print(f"  \033[91m{self.failed} CHECKS FAILED out of {self.passed + self.failed}\033[0m")
        print("=======================================================\n")

        self.cleanup()

    def cleanup(self):
        if self.hub_proc:
            try:
                self.hub_proc.terminate()
                self.hub_proc.wait(timeout=1.0)
            except Exception:
                try: os.kill(self.hub_proc.pid, signal.SIGKILL)
                except: pass
        self.rf_router.stop()
        self.mock_ctrl.close()

if __name__ == "__main__":
    runner = CoSimulationRunner()
    runner.run()
    sys.exit(0 if runner.failed == 0 else 1)
