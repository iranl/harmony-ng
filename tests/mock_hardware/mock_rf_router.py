#!/usr/bin/env python3
"""
CC2544 Virtual RF Air Interface Router.
Bridges Hub (Report 0x12, 30 bytes) and Remote (Report 0x20, 32 bytes).
Simulates channel hopping, packet ACKs, slot mapping, and framing translations.
"""

import os
import sys
import time
import socket
import select
import threading
import logging

logging.basicConfig(level=logging.INFO, format="[RF-ROUTER %(asctime)s] %(message)s", datefmt="%H:%M:%S")
log = logging.getLogger("rf_router")

DEFAULT_MOCK_DIR = os.getenv("HARMONY_MOCK_DIR", "/tmp/harmony_mock")

class CC2544VirtualRouter:
    def __init__(self, mock_dir=DEFAULT_MOCK_DIR):
        self.mock_dir = mock_dir
        os.makedirs(self.mock_dir, exist_ok=True)
        self.hub_sock_path = os.path.join(self.mock_dir, "hub_rf.sock")
        self.elite_sock_path = os.path.join(self.mock_dir, "elite_rf.sock")

        self.running = False
        self.hub_server = None
        self.elite_server = None
        self.hub_client = None
        self.elite_client = None

        self.hub_tx_count = 0
        self.remote_tx_count = 0
        self.packet_log = []
        self.simulate_drop_rate = 0.0
        self.lock = threading.Lock()

    def start(self):
        # Remove old socket files if present
        for p in (self.hub_sock_path, self.elite_sock_path):
            if os.path.exists(p):
                os.unlink(p)

        self.hub_server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.hub_server.bind(self.hub_sock_path)
        self.hub_server.listen(1)

        self.elite_server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.elite_server.bind(self.elite_sock_path)
        self.elite_server.listen(1)

        self.running = True
        log.info(f"CC2544 Virtual RF Router online.")
        log.info(f"  Hub socket:   {self.hub_sock_path}")
        log.info(f"  Elite socket: {self.elite_sock_path}")

        self.worker_thread = threading.Thread(target=self._run_loop, daemon=True)
        self.worker_thread.start()

    def stop(self):
        self.running = False
        if self.hub_client:
            try: self.hub_client.close()
            except: pass
        if self.elite_client:
            try: self.elite_client.close()
            except: pass
        if self.hub_server:
            try: self.hub_server.close()
            except: pass
        if self.elite_server:
            try: self.elite_server.close()
            except: pass
        for p in (self.hub_sock_path, self.elite_sock_path):
            if os.path.exists(p):
                try: os.unlink(p)
                except: pass
        log.info("CC2544 Virtual RF Router stopped.")

    def _run_loop(self):
        while self.running:
            readable = [self.hub_server, self.elite_server]
            if self.hub_client: readable.append(self.hub_client)
            if self.elite_client: readable.append(self.elite_client)

            try:
                r, _, _ = select.select(readable, [], [], 0.1)
            except Exception:
                break

            for s in r:
                if s is self.hub_server:
                    client, _ = self.hub_server.accept()
                    client.setblocking(False)
                    self.hub_client = client
                    log.info("[+] Hub CC2544 connected")
                elif s is self.elite_server:
                    client, _ = self.elite_server.accept()
                    client.setblocking(False)
                    self.elite_client = client
                    log.info("[+] Remote CC2544 connected")
                elif s is self.hub_client:
                    self._handle_hub_traffic()
                elif s is self.elite_client:
                    self._handle_remote_traffic()

    def _handle_hub_traffic(self):
        try:
            data = self.hub_client.recv(1024)
            if not data:
                log.info("[-] Hub disconnected")
                self.hub_client.close()
                self.hub_client = None
                return
        except Exception:
            return

        idx = 0
        while idx < len(data):
            # Hub frames are 30 bytes Report 0x12
            frame = data[idx:idx+30]
            if len(frame) < 30:
                break
            idx += 30

            report_id = frame[0]
            slot = frame[1]
            hdr = frame[2]
            msg_type = frame[3]
            flags = frame[4]
            payload = frame[5:30]

            with self.lock:
                self.hub_tx_count += 1
                self.packet_log.append({
                    "src": "hub", "slot": slot, "hdr": hdr,
                    "msg_type": hex(msg_type), "payload": list(payload)
                })

            log.info(f"Hub -> Slot {slot} (msg=0x{msg_type:02X}, hdr=0x{hdr:02X})")

            # Route to Remote: Format as 32-byte Report 0x20
            # [0x20] [0x21] [0x12] [0x01] [hdr_flag] [msg_type] [flags] [payload (25B)]
            if self.elite_client:
                remote_frame = bytearray(32)
                remote_frame[0] = 0x20
                remote_frame[1] = 0x21
                remote_frame[2] = 0x12
                remote_frame[3] = 0x01
                remote_frame[4] = hdr
                remote_frame[5] = msg_type
                remote_frame[6] = flags
                remote_frame[7:32] = payload
                try:
                    self.elite_client.sendall(remote_frame)
                except Exception as e:
                    log.warning(f"Failed delivering to Elite: {e}")

    def _handle_remote_traffic(self):
        try:
            data = self.elite_client.recv(1024)
            if not data:
                log.info("[-] Remote disconnected")
                self.elite_client.close()
                self.elite_client = None
                return
        except Exception:
            return

        idx = 0
        while idx < len(data):
            # Check for 10-byte Active Dongle Sync pulse
            if len(data) - idx == 10 and data[idx] == 0x12 and data[idx+1] == 0x01:
                log.info("Remote sync pulse received (10B)")
                idx += 10
                continue

            # Remote frames are 32 bytes Report 0x20
            frame = data[idx:idx+32]
            if len(frame) < 32:
                break
            idx += 32

            # Unpack Report 0x20
            # Byte 0: 0x20, Byte 1: 0x21, Byte 2: 0x12, Byte 3: target pipe (0x01)
            # Byte 4: hdr_flag, Byte 5: msg_type, Byte 6: flags, Byte 7..31: payload (25B)
            hdr = frame[4]
            msg_type = frame[5]
            flags = frame[6]
            payload = frame[7:32]

            with self.lock:
                self.remote_tx_count += 1
                self.packet_log.append({
                    "src": "remote", "hdr": hdr,
                    "msg_type": hex(msg_type), "payload": list(payload)
                })

            log.info(f"Remote -> Hub (msg=0x{msg_type:02X}, hdr=0x{hdr:02X})")

            # Route to Hub: Format as 30-byte Report 0x12
            # [0x12] [slot 0x01] [hdr_flag] [msg_type] [flags] [payload (25B)]
            if self.hub_client:
                hub_frame = bytearray(30)
                hub_frame[0] = 0x12
                hub_frame[1] = 0x01  # Slot 1
                hub_frame[2] = hdr
                hub_frame[3] = msg_type
                hub_frame[4] = flags
                hub_frame[5:30] = payload
                try:
                    self.hub_client.sendall(hub_frame)
                except Exception as e:
                    log.warning(f"Failed delivering to Hub: {e}")

if __name__ == "__main__":
    router = CC2544VirtualRouter()
    router.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        router.stop()
