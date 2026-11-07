#!/usr/bin/env python3
"""All-in-one setup tool for Logitech Harmony touchscreen remotes (Elite / Ultimate / 950).

Transforms a stock/factory remote into a permanently rooted device with:
- Persistent USB Ethernet on every boot (no touchscreen needed)
- Hardware watchdog disabled (no kernel panic reboots)
- Dropbear SSH server on 192.168.2.1:22 with public-key authentication

Usage:
    python tools/setup_remote.py
    python tools/setup_remote.py --step 1   (USB HID debug unlock only)
    python tools/setup_remote.py --step 2   (Dropbear & persistence install only)
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tarfile
import time
import urllib.request

REMOTE_IP = "192.168.2.1"
REMOTE_PORT = 8088
REMOTE_USB_PID = 0xC12B
REMOTE_USB_VID = 0x046D

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR.parent

if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools.usb_state import (
    detect_remote_usb_state,
    print_usb_state_banner,
    require_firmware_or_exit,
    UsbMode,
    UsbState,
)


def find_candidate(candidates: list[pathlib.Path]) -> pathlib.Path:
    for c in candidates:
        if c.is_file():
            return c
    return candidates[0]


def find_dir_candidate(candidates: list[pathlib.Path]) -> pathlib.Path:
    for c in candidates:
        if c.is_dir():
            return c
    return candidates[0]


TINY_PATH = find_candidate([
    REPO_DIR / "bin" / "b64dec_tiny",
    REPO_DIR / "remote" / "bin" / "b64dec_tiny",
    REPO_DIR / "remote" / "b64dec_tiny",
    SCRIPT_DIR / "bin" / "b64dec_tiny",
    SCRIPT_DIR / "b64dec_tiny",
])

DROPBEAR_PATH = find_candidate([
    REPO_DIR / "bin" / "dropbearmulti_musl",
    REPO_DIR / "remote" / "bin" / "dropbearmulti_musl",
    SCRIPT_DIR / "bin" / "dropbearmulti_musl",
    SCRIPT_DIR / "dropbearmulti_musl",
])
DEFAULT_KEY_PATH = None

# Backup of the rooted remote, before the HarmonyNG runtime is installed
BACKUP_DIR = REPO_DIR / "backups" / "remote"
BACKUP_PATHS = ["data", "opt/luadraws", "etc"]
# BusyBox binaries on the remote that may have a tar applet (stock /bin/busybox has none)
REMOTE_BUSYBOX_CANDIDATES = ["/data/busybox", "/tmp/busybox", "/bin/busybox"]

LUA_DECODER_ONELINE = (
    "local c='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/' "
    "local lut={} for i=1,64 do lut[c:byte(i)]=i-1 end "
    "local function d(s) local o,n,l,i={},0,#s,1 while i<=l do "
    "local b1,b2,b3,b4=lut[s:byte(i)],lut[s:byte(i+1)],lut[s:byte(i+2)],lut[s:byte(i+3)] "
    "if not b1 or not b2 then break end "
    "n=n+1 o[n]=string.char((b1*4)+math.floor(b2/16)) "
    "if b3 then n=n+1 o[n]=string.char(((b2%16)*16)+math.floor(b3/4)) "
    "if b4 then n=n+1 o[n]=string.char(((b3%4)*64)+b4) end end "
    "i=i+4 end return table.concat(o) end "
    "local f,g,r=io.open(arg[1],'r'),io.open(arg[2],'wb'),'' "
    "while true do local k=f:read(16384) if not k then break end "
    "k=r..k:gsub('%s+','') local t=#k-(#k%4) if t>0 then g:write(d(k:sub(1,t))) "
    "r=k:sub(t+1) else r=k end end "
    "if #r>0 then g:write(d(r)) end f:close() g:close()"
)


# ==============================================================================
# Helper functions
# ==============================================================================

def rce(cmd: str, timeout: int = 15) -> None:
    """Execute arbitrary root command via unescaped rm -rf injection on port 8088."""
    body = {
        "id": 1,
        "cmd": "setup.firmware?packageremove",
        "params": {"name": "; " + cmd},
    }
    req = urllib.request.Request(
        f"http://{REMOTE_IP}:{REMOTE_PORT}/",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Origin": "http://sl.myharmony.com",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        pass


def read_json_val(remote_file: str, key: str, timeout: int = 5) -> str:
    """Read a JSON key from a remote file via connect.jsonfiletransfer?get."""
    parts = remote_file.strip("/").split("/")
    folder = "../../" + "/".join(parts[:-1]) if len(parts) > 1 else "../.."
    filename = parts[-1]
    body = {
        "id": 2,
        "cmd": "connect.jsonfiletransfer?get",
        "params": {"path": folder, "file": filename},
    }
    req = urllib.request.Request(
        f"http://{REMOTE_IP}:{REMOTE_PORT}/",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Origin": "http://sl.myharmony.com",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        res = json.loads(r.read().decode("utf-8"))
        return res.get("data", {}).get(key, "")


def probe_remote(ip: str = REMOTE_IP, port: int = REMOTE_PORT, timeout: float = 1.0) -> bool:
    """Check if remote HTTP port is reachable."""
    s = socket.socket()
    s.settimeout(timeout)
    try:
        s.connect((ip, port))
        s.close()
        return True
    except Exception:
        return False


BOOTSTRAP_SCRIPT_TEMPLATE = b"""#!/bin/sh
# Auto-generated by Harmony Remote USB Root Tool
# Phase 1: Start USB Ethernet gadget
killall -9 usbhid 2>/dev/null || true
chmod -x /usr/bin/usbhid 2>/dev/null || true
/usr/bin/usbeth start 2>/dev/null || true

# Phase 2: Install SSH authorized keys
mkdir -p /etc/dropbear /root/.ssh /home/root/.ssh
cat <<'EOF' > /etc/dropbear/authorized_keys
%PUBKEY%
EOF
cp /etc/dropbear/authorized_keys /root/.ssh/authorized_keys
cp /etc/dropbear/authorized_keys /home/root/.ssh/authorized_keys
chmod 700 /root /root/.ssh /home/root /home/root/.ssh
chmod 600 /etc/dropbear/authorized_keys /root/.ssh/authorized_keys /home/root/.ssh/authorized_keys

# Phase 3: Disable hardware watchdog
touch /etc/nowatchdog
chmod 644 /etc/nowatchdog

# Phase 4: Extract Dropbear multi-call binary using stock Lua
/usr/bin/lua -e 'local f=io.open("/usr/sbin/dropbear","rb") or io.open("/usr/sbin/dropbearmulti","rb") f:seek("set",%OFFSET%) local g=io.open("/data/dropbearmulti","wb") while true do local b=f:read(65536) if not b then break end g:write(b) end f:close() g:close()'

chmod 755 /data/dropbearmulti
ln -sf /data/dropbearmulti /usr/sbin/dropbear
ln -sf /data/dropbearmulti /usr/sbin/dropbearkey
ln -sf /data/dropbearmulti /usr/sbin/dbclient
ln -sf /data/dropbearmulti /usr/sbin/scp

# Phase 5: Ensure RSA host key exists
if [ ! -r /etc/dropbear/dropbear_rsa_host_key ]; then
    /data/dropbearmulti dropbearkey -t rsa -f /etc/dropbear/dropbear_rsa_host_key
fi

# Phase 6: Ensure boot persistence in /etc/init.d/rcS.local
killall -9 remote_ui remote_watchdog.sh watchdog hal luaworks luadraws 2>/dev/null || true
printf '#!/bin/sh\nkillall -9 usbhid 2>/dev/null\n/usr/bin/usbeth start &\n/data/dropbearmulti dropbear -R -p 0.0.0.0:22\nexit 0\n' > /etc/init.d/rcS.local
chmod 755 /etc/init.d/rcS.local
rm -f /data/remote_ui /data/remote_watchdog.sh 2>/dev/null || true

# Phase 7: Start Dropbear SSH daemon and terminate parent rcS
/data/dropbearmulti dropbear -R -p 0.0.0.0:22 "$@"
kill -9 $PPID 2>/dev/null || true
exit 0
"""


def build_payload(pubkey_str: str) -> bytes:
    dropbear_path = find_candidate([
        REPO_DIR / "bin" / "dropbearmulti_musl",
        REPO_DIR / "remote" / "bin" / "dropbearmulti_musl",
        SCRIPT_DIR / "bin" / "dropbearmulti_musl",
        SCRIPT_DIR / "dropbearmulti_musl",
    ])
    if not dropbear_path.is_file():
        raise FileNotFoundError(f"ARM Dropbear binary missing at {dropbear_path}")

    arm_binary = dropbear_path.read_bytes()
    dummy_offset = b"00000000"
    base_script = BOOTSTRAP_SCRIPT_TEMPLATE.replace(b"%PUBKEY%", pubkey_str.strip().encode("ascii"))
    template = base_script.replace(b"%OFFSET%", dummy_offset)
    actual_offset = len(template)

    final_offset_str = f"{actual_offset:08d}".encode("ascii")
    script = base_script.replace(b"%OFFSET%", final_offset_str)
    assert len(script) == actual_offset, f"Script length mismatch: {len(script)} vs {actual_offset}"

    return script + arm_binary


def probe_tcp(ip: str, port: int, timeout_s: float = 1.0) -> bool:
    s = socket.socket()
    s.settimeout(timeout_s)
    try:
        s.connect((ip, port))
        s.close()
        return True
    except Exception:
        return False


def wait_for_ssh(ip: str = REMOTE_IP, port: int = 22, timeout_s: int = 60) -> bool:
    t_end = time.time() + timeout_s
    while time.time() < t_end:
        if probe_tcp(ip, port, timeout_s=1.0):
            return True
        time.sleep(1.0)
        print(".", end="", flush=True)
    return False


# ==============================================================================
# Backup (stock BusyBox has no tar: stage the project's own BusyBox in RAM)
# ==============================================================================

def remote_ssh_cmd(priv_path: pathlib.Path, ip: str = REMOTE_IP) -> list[str]:
    return [
        "ssh",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=10",
        "-o", "LogLevel=ERROR",
        "-i", str(priv_path),
        f"root@{ip}",
    ]


def find_local_arm_busybox() -> pathlib.Path | None:
    """The project's static ARM BusyBox: build/build_busybox.sh arm, or bin/ of the remote release bundle."""
    for cand in (
        REPO_DIR / "remote" / "bin" / "busybox",
        REPO_DIR / "bin" / "busybox",
        SCRIPT_DIR / "bin" / "busybox",
    ):
        try:
            with open(cand, "rb") as f:
                head = f.read(20)
        except OSError:
            continue
        # 32-bit little-endian ELF for ARM (e_machine 0x28): never stage the Hub's MIPS build
        if head[:4] == b"\x7fELF" and head[4] == 1 and head[5] == 1 and head[18:20] == b"\x28\x00":
            return cand
    return None


def find_remote_tar_busybox(ssh: list[str], candidates: list[str] | None = None) -> str:
    """Return the first BusyBox on the remote that has the tar applet, or ''."""
    cands = " ".join(candidates or REMOTE_BUSYBOX_CANDIDATES)
    probe = (
        f"for b in {cands}; do "
        # stock BusyBox grep has no -x
        '[ -x "$b" ] && "$b" --list 2>/dev/null | grep -q "^tar$" && { echo "$b"; exit 0; }; '
        "done; true"
    )
    res = subprocess.run(ssh + [probe], capture_output=True, text=True, timeout=30, check=False)
    return res.stdout.strip()


def stage_busybox(ssh: list[str], local_bb: pathlib.Path) -> str:
    """Copy local_bb to /tmp/busybox (tmpfs, gone after a reboot); return its path or '' on failure."""
    print(f"[*] Staging the project's BusyBox ({local_bb.stat().st_size:,} bytes) as /tmp/busybox (RAM)...")
    with open(local_bb, "rb") as f:
        res = subprocess.run(ssh + ["cat > /tmp/busybox && chmod 755 /tmp/busybox"], stdin=f,
                             capture_output=True, timeout=120, check=False)
    if res.returncode != 0 or find_remote_tar_busybox(ssh, ["/tmp/busybox"]) != "/tmp/busybox":
        print("[-] Could not stage /tmp/busybox on the remote.")
        subprocess.run(ssh + ["rm -f /tmp/busybox"], capture_output=True, timeout=30, check=False)
        return ""
    return "/tmp/busybox"


def backup_remote(priv_path: pathlib.Path, ip: str = REMOTE_IP) -> pathlib.Path | None:
    """Stream /data, /opt/luadraws and /etc of the remote to backups/remote/ (plain tar)."""
    print("\n" + "=" * 70)
    print(" [*] BACKING UP THE REMOTE (/data, /opt/luadraws, /etc)")
    print("=" * 70)
    ssh = remote_ssh_cmd(priv_path, ip)
    staged = False
    try:
        bb = find_remote_tar_busybox(ssh)
        if not bb:
            local_bb = find_local_arm_busybox()
            if not local_bb:
                print("[-] The remote's BusyBox has no tar, and no ARM BusyBox was found locally")
                print("    (build it with ./build/build_busybox.sh arm). Backup skipped.")
                return None
            bb = stage_busybox(ssh, local_bb)
            if not bb:
                return None
            staged = True
        else:
            print(f"[*] Using {bb} on the remote.")

        BACKUP_DIR.mkdir(parents=True, exist_ok=True)
        out = BACKUP_DIR / f"remote_backup_{time.strftime('%Y%m%d_%H%M%S')}.tar"
        tar_cmd = (
            f"cd / && p=''; for d in {' '.join(BACKUP_PATHS)}; do [ -e \"$d\" ] && p=\"$p $d\"; done; "
            f"{bb} tar -cf - $p 2>/dev/null"
        )
        t0 = time.time()
        with open(out, "wb") as f:
            res = subprocess.run(ssh + [tar_cmd], stdout=f, stderr=subprocess.PIPE, timeout=900, check=False)
        try:
            with tarfile.open(out, "r:") as t:
                entries = len(t.getnames())
        except (tarfile.TarError, OSError) as e:
            print(f"[-] Backup is not a valid tar archive ({e}); removed.")
            out.unlink(missing_ok=True)
            return None
        if entries == 0:
            print("[-] Backup is empty; removed.")
            out.unlink(missing_ok=True)
            return None
        size_mb = out.stat().st_size / (1024 * 1024)
        note = "" if res.returncode == 0 else f", tar exit code {res.returncode}: some files could not be read"
        print(f"[+] Backup saved: {out} ({size_mb:.1f} MB, {entries} entries, {time.time() - t0:.0f} s{note})")
        golden = BACKUP_DIR / "golden_remote_backup.tar"
        if not golden.exists():
            shutil.copyfile(out, golden)
            print(f"[+] First backup also kept as {golden.name}")
        return out
    except (OSError, subprocess.SubprocessError) as e:
        print(f"[-] Backup failed: {e}")
        return None
    finally:
        if staged:
            subprocess.run(ssh + ["rm -f /tmp/busybox"], capture_output=True, timeout=30, check=False)


# ==============================================================================
# STEP 1: Stage Dropbear & Enable Debug Mode over USB HID
# ==============================================================================

def step1_enable_usb_debug(pubkey_str: str, skip_fw_check: bool = False) -> bool:
    print("\n" + "=" * 70)
    print(" [STEP 1/2] STAGING DROPBEAR SSH OVER USB HID")
    print("=" * 70)

    # Import clean-room Harmony HID driver
    if str(REPO_DIR) not in sys.path:
        sys.path.insert(0, str(REPO_DIR))

    try:
        from tools.harmony_hid import HarmonyHid, REMOTE_PID
    except ImportError as e:
        print(f"[-] Error: Could not import tools.harmony_hid: {e}")
        return False

    print("[*] Connecting to Harmony Remote over USB HID...")
    time.sleep(0.3)
    hid = None

    for pid in (REMOTE_PID, 0xC132):
        h = HarmonyHid(pid=pid)
        if h.open():
            hid = h
            print(f"[+] Connected to Harmony Remote over USB HID (046D:{pid:04X})")
            break

    if not hid:
        print("[-] USB connection failed.")
        print("    Ensure remote is plugged in via USB and powered ON.")
        return False

    try:
        if not skip_fw_check:
            # 1. Mandatory firmware version check & display over USB HID
            current_fw, target_skin = require_firmware_or_exit(hid)
            from tools.harmony_firmware import SKIN_NAMES
            skin_name = SKIN_NAMES.get(target_skin, f"Harmony Remote (Skin {target_skin})")
            print(f"[*] Detected Remote Hardware: {skin_name}")
            print(f"[*] Current Firmware Version: {current_fw}")
            print("[*] Checking official Logitech SUS update servers...")
            from tools.harmony_firmware import (
                DEFAULT_ELITE_SKIN,
                query_latest_firmware,
                download_file,
                parse_hfw2_bundle,
                flash_firmware,
            )
            res = query_latest_firmware(target_skin, silent_on_empty=True)
            if (not res or not res.get("Id")) and target_skin != DEFAULT_ELITE_SKIN:
                # Some skins (e.g. 100, Harmony Ultimate 2013) have no entry of their own;
                # the remote bundle published for skin 111 lists them as intended devices
                print(f"[*] No firmware listed for skin {target_skin}, checking skin {DEFAULT_ELITE_SKIN} "
                      f"(same remote firmware bundle)")
                res = query_latest_firmware(DEFAULT_ELITE_SKIN)
            if res and res.get("Id"):

                latest_fw = res["Id"]
                if current_fw != latest_fw:
                    print(f"\n[!] Remote firmware is {current_fw}, but latest official firmware is {latest_fw}.")
                    ans = input(f"[?] Download and install latest official firmware ({latest_fw}) first? (y/N): ")
                    if ans.strip().lower() == "y":
                        fw_dir = REPO_DIR / "firmware"
                        fw_file = fw_dir / f"remote_{target_skin}_{latest_fw}.hfw2"
                        if not fw_file.is_file():
                            if not download_file(res["URI"], fw_file, progress_desc=f"Downloading {fw_file.name}"):
                                print("[-] Firmware download failed. Proceeding with current firmware...")
                        if fw_file.is_file():
                            bundle = parse_hfw2_bundle(fw_file)
                            print("[*] Installing latest firmware over USB HID...")
                            hid.close()
                            if flash_firmware(bundle, target_device="remote", skip_confirm=True):
                                print("[*] Waiting 30s for Remote to complete reboot and flash...")
                                time.sleep(15)
                                t_reconnect = time.time() + 60
                                hid = HarmonyHid(pid=REMOTE_PID)
                                while time.time() < t_reconnect:
                                    if hid.open():
                                        print("\n[+] Remote re-enumerated on USB after firmware upgrade!")
                                        break
                                    time.sleep(2)
                                    print(".", end="", flush=True)
                                else:
                                    print("\n[-] Timed out waiting for Remote to re-enumerate on USB after upgrade.")
                                    sys.exit(1)
                else:
                    print(f"[+] Remote is running the latest official firmware ({latest_fw}).")

        payload = build_payload(pubkey_str)
        print(f"[+] Self-extracting ARM Dropbear payload built: {len(payload):,} bytes")
        print(f"[*] Staging Dropbear payload to /sys/dropbear ({len(payload):,} bytes)...")
        t0 = time.time()

        def progress(sent: int, total: int) -> None:
            pct = int(sent / total * 100)
            print(f"\r    Uploaded {sent:,} / {total:,} bytes ({pct}%)", end="", flush=True)

        hid.write_full_file("/sys/dropbear", payload, packets_per_chunk=5, progress_callback=progress)
        print(f"\n[+] Dropbear staging complete in {time.time() - t0:.1f}s.")

        print("[*] Arming boot trigger (/tde/enable -> /etc/tdeenable)...")
        if hid.set_tde_enable(True):
            print("[+] SUCCESS: /etc/tdeenable created on remote.")
        else:
            print("[-] Warning: Failed to confirm /tde/enable.")

        print("[*] Waiting 3s for remote filesystem buffers to flush to flash...")
        time.sleep(3)

        print("[*] Sending reboot command to /sys/reboot...")
        try:
            hid.reboot()
            print("[+] Remote is rebooting into rooted firmware multi-user mode!")
        except Exception as e:
            print(f"[-] Reboot exception (expected if device disconnects): {e}")
        return True
    except Exception as e:
        print(f"[-] Error over USB HID: {e}")
        return False
    finally:
        hid.close()


def check_and_fix_windows_driver() -> bool:
    """If running on Windows and RNDIS driver or IP is missing, launch setup_remote_driver.ps1 elevated."""
    if sys.platform != "win32":
        return False
    usb_st = detect_remote_usb_state(check_firmware=False)
    if usb_st.mode == UsbMode.DRIVER_ERROR or (usb_st.mode == UsbMode.USB_ETHERNET and not usb_st.ip_valid):
        print(f"\n[!] Windows driver or network configuration needed ({usb_st.description or 'RNDIS Gadget'}).")
        print("[*] Requesting Administrator elevation to install RNDIS driver & configure static IP...")
        ps_script = REPO_DIR / "tools" / "setup_remote_driver.ps1"
        if ps_script.is_file():
            try:
                subprocess.run([
                    "powershell.exe", "-NoProfile", "-Command",
                    f"Start-Process powershell.exe -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"{ps_script}\" -NonInteractive' -Wait"
                ])
                time.sleep(2)
                return True
            except Exception as e:
                print(f"[-] Failed to launch setup_remote_driver.ps1: {e}")
        else:
            print(f"[-] Could not find {ps_script}")
    return False


def print_step1_instructions() -> None:
    print("\n" + "=" * 70)
    print(" CONFIGURE STATIC IP ON PC:")
    print("=" * 70)
    if sys.platform == "win32":
        print("   - Run the automated setup tool (elevated):")
        print("        powershell -ExecutionPolicy Bypass -File tools/setup_remote_driver.ps1")
        print("   - Or manually configure Network Connections (Win+R -> ncpa.cpl):")
        print("        Right-click 'Remote NDIS / Ethernet Gadget' -> Properties -> IPv4:")
        print("        IP Address:      192.168.2.2")
        print("        Subnet Mask:     255.255.255.0")
        print("        Default Gateway: (leave blank)")
    elif sys.platform == "darwin":
        print("   - macOS adds a network service named 'RNDIS/Ethernet Gadget' (no driver needed).")
        print("   - Give it a static address (no router, so your internet route is unchanged):")
        print('        sudo networksetup -setmanual "RNDIS/Ethernet Gadget" 192.168.2.2 255.255.255.0')
        print("   - To undo later:")
        print('        sudo networksetup -setdhcp "RNDIS/Ethernet Gadget"')
    else:
        print("   - A USB Ethernet interface (e.g. usb0 or enx...) will appear; find it with: ip link")
        print("   - Give it a static address:")
        print("        sudo ip addr add 192.168.2.2/24 dev <interface>")
        print("        sudo ip link set <interface> up")
    print("=" * 70)


def wait_for_ethernet() -> bool:
    print(f"\n[*] Probing remote at http://{REMOTE_IP}:{REMOTE_PORT}/...")
    print("    (Waiting for USB Ethernet connection to become active. Press Ctrl+C to cancel)")
    driver_checked = False
    for i in range(120):
        if probe_remote():
            print(f"\n[+] Connected! Remote HTTP service detected at {REMOTE_IP}:{REMOTE_PORT}")
            return True
        if sys.platform == "win32" and not driver_checked and i >= 5:
            driver_checked = True
            check_and_fix_windows_driver()
        time.sleep(1)
        print(".", end="", flush=True)
    print("\n[-] Timeout waiting for remote network interface.")
    return False


# ==============================================================================
# SSH Key Management
# ==============================================================================

def resolve_default_key_path() -> pathlib.Path:
    """Find most recently modified harmony_owner_* key in ~/.ssh or default to harmony_owner_ed25519."""
    ssh_dir = pathlib.Path.home() / ".ssh"
    if ssh_dir.is_dir():
        keys = [
            p
            for p in ssh_dir.glob("harmony_owner_*")
            if p.is_file() and not p.name.endswith(".pub")
        ]
        if keys:
            return sorted(keys, key=lambda p: p.stat().st_mtime, reverse=True)[0]
    return ssh_dir / "harmony_owner_ed25519"


def ensure_ssh_key(key_path: pathlib.Path | None = None) -> pathlib.Path:
    """Resolve existing harmony_owner key or generate a new ED25519 keypair."""
    if key_path is None:
        key_path = resolve_default_key_path()

    pub_path = key_path.with_suffix(".pub")
    if key_path.exists() and pub_path.exists():
        print(f"[*] Reusing existing Harmony owner key: {key_path}")
        return pub_path

    print(f"[*] No existing key found. Generating new Harmony owner keypair at {key_path}...")
    key_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "ssh-keygen",
        "-t", "ed25519",
        "-f", str(key_path),
        "-N", "",
        "-C", "harmony-owner",
    ]
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        print("[-] Error: ssh-keygen not found in system PATH.")
        sys.exit(1)

    print(f"[+] Keypair created: {pub_path.name}")
    return pub_path



def update_ssh_config(key_path: pathlib.Path) -> None:
    """Optionally add Host entry to ~/.ssh/config."""
    config_path = pathlib.Path.home() / ".ssh" / "config"
    config_entry = (
        f"\nHost {REMOTE_IP} harmony-remote\n"
        f"    HostName {REMOTE_IP}\n"
        f"    User root\n"
        f"    IdentityFile {key_path}\n"
        f"    StrictHostKeyChecking accept-new\n"
    )
    try:
        existing = config_path.read_text(encoding="utf-8") if config_path.exists() else ""
        if REMOTE_IP not in existing:
            config_path.parent.mkdir(parents=True, exist_ok=True)
            with open(config_path, "a", encoding="utf-8") as f:
                f.write(config_entry)
            print(f"[+] Added '{REMOTE_IP}' to {config_path}")
    except Exception as e:
        print(f"[*] Note: Could not update {config_path}: {e}")


# ==============================================================================
# STEP 2: Install Dropbear & Persistence
# ==============================================================================

def step2_install_permanent_ssh(pub_path: pathlib.Path, no_screen: bool = False, no_backup: bool = False) -> bool:
    print("\n" + "=" * 70)
    print(" [STEP 2/2] INSTALLING PERSISTENT DROPBEAR SSH & USB ETHERNET")
    print("=" * 70)

    if not TINY_PATH.exists():
        print(f"[-] Error: Native ARM decoder missing at {TINY_PATH}")
        return False
    if not DROPBEAR_PATH.exists():
        print(f"[-] Error: musl Dropbear binary missing at {DROPBEAR_PATH}")
        return False

    # 1. Disable hardware watchdog permanently
    print("[*] Disabling hardware watchdog (/etc/nowatchdog)...")
    rce("touch /etc/nowatchdog; chmod 644 /etc/nowatchdog")

    # 2. Stage native ARM base64 decoder
    tiny_bytes = TINY_PATH.read_bytes()
    tiny_b64 = base64.b64encode(tiny_bytes).decode("ascii")
    print(f"[*] Staging native ARM base64 decoder ({len(tiny_bytes)} bytes)...")
    rce(f'echo "{LUA_DECODER_ONELINE}" > /tmp/bootstrap.lua')
    rce(f'echo "{tiny_b64}" > /tmp/tiny.b64')
    rce("/usr/bin/lua /tmp/bootstrap.lua /tmp/tiny.b64 /tmp/b64dec_tiny")
    rce("chmod 755 /tmp/b64dec_tiny; rm -f /tmp/bootstrap.lua /tmp/tiny.b64")

    # Verify b64dec_tiny
    rce('sz=$(ls -l /tmp/b64dec_tiny 2>/dev/null | awk "{print \\$5}"); echo "{\\"sz\\": \\"$sz\\"}" > /tmp/tinysz.json')
    actual_tiny_sz = read_json_val("/tmp/tinysz.json", "sz")
    if actual_tiny_sz != str(len(tiny_bytes)):
        print(f"[-] Decoder staging failed (got {actual_tiny_sz}, expected {len(tiny_bytes)})")
        return False
    print("[+] Native ARM decoder verified.")

    # 3. Stream Dropbear binary in chunks
    dropbear_bytes = DROPBEAR_PATH.read_bytes()
    expected_sz = len(dropbear_bytes)
    db_b64 = base64.b64encode(dropbear_bytes).decode("ascii")
    chunk_sz = 8192
    total_chunks = math.ceil(len(db_b64) / chunk_sz)

    print(f"[*] Uploading musl Dropbear binary ({expected_sz:,} bytes in {total_chunks} chunks)...")
    rce("rm -f /tmp/dropbear.b64 /data/dropbearmulti")
    t0 = time.time()
    for i in range(total_chunks):
        chunk = db_b64[i * chunk_sz : (i + 1) * chunk_sz]
        rce(f"echo -n '{chunk}' >> /tmp/dropbear.b64")
        if (i + 1) % 15 == 0 or (i + 1) == total_chunks:
            pct = int((i + 1) / total_chunks * 100)
            print(f"    Uploaded {i + 1}/{total_chunks} chunks ({pct}%)")

    # 4. Decode binary on device in milliseconds
    print("[*] Decoding binary with native ARM decoder...")
    rce("/tmp/b64dec_tiny /tmp/dropbear.b64 /data/dropbearmulti")
    rce("rm -f /tmp/dropbear.b64 /tmp/b64dec_tiny")

    # Verify Dropbear binary size
    rce('sz=$(ls -l /data/dropbearmulti 2>/dev/null | awk "{print \\$5}"); echo "{\\"sz\\": \\"$sz\\"}" > /tmp/dbsz.json')
    actual_db_sz = read_json_val("/tmp/dbsz.json", "sz")
    if actual_db_sz != str(expected_sz):
        print(f"[-] Dropbear size mismatch (got {actual_db_sz}, expected {expected_sz})")
        return False
    print(f"[+] Dropbear multi-call binary verified ({actual_db_sz} bytes).")

    # 5. Create symlinks and directories
    print("[*] Configuring symlinks and hostkeys...")
    setup_cmds = [
        "chmod 755 /data/dropbearmulti",
        "mkdir -p /data/bin /etc/dropbear /home/root/.ssh /root/.ssh",
        "ln -sf /data/dropbearmulti /data/bin/dropbear",
        "ln -sf /data/dropbearmulti /data/bin/dropbearkey",
        "ln -sf /data/dropbearmulti /usr/sbin/dropbear",
        "ln -sf /data/dropbearmulti /usr/sbin/dropbearkey",
        "ln -sf /data/dropbearmulti /usr/sbin/dbclient",
        "ln -sf /data/dropbearmulti /usr/sbin/scp",
        "[ -f /etc/dropbear/dropbear_rsa_host_key ] || /usr/sbin/dropbearkey -t rsa -f /etc/dropbear/dropbear_rsa_host_key",
        "[ -f /etc/dropbear/dropbear_ed25519_host_key ] || /usr/sbin/dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key",
        "chmod 700 /home/root /home/root/.ssh /root /root/.ssh",
    ]
    rce("; ".join(setup_cmds))

    # 6. Install public key
    pubkey = pub_path.read_text("utf-8").strip()
    print(f"[*] Installing public key ({pubkey.split()[-1]})...")
    key_cmds = [
        f"echo '{pubkey}' > /home/root/.ssh/authorized_keys",
        f"echo '{pubkey}' > /root/.ssh/authorized_keys",
        f"echo '{pubkey}' > /etc/dropbear/authorized_keys",
        "chmod 600 /home/root/.ssh/authorized_keys /root/.ssh/authorized_keys /etc/dropbear/authorized_keys",
    ]
    rce("; ".join(key_cmds))

    # 7. Make persistent in /etc/init.d/rcS.local
    print("[*] Preserving stock configuration backup...")
    rce("mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'elite' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'remote_ui' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'usbeth' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'dropbear' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'exit 0' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; fi; fi")
    print("[*] Installing boot persistence (/etc/init.d/rcS.local)...")
    rcs = "#!/bin/sh\\n/usr/bin/usbeth start &\\n/usr/sbin/dropbear -R -p 0.0.0.0:22\\n"
    rce(f"echo -e '{rcs}' > /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local")

    # 8. Start Dropbear daemon
    print("[*] Launching Dropbear SSH daemon on port 22...")
    rce("killall -9 dropbearmulti dropbear 2>/dev/null; /usr/sbin/dropbear -R -p 0.0.0.0:22")
    time.sleep(1)

    # 9. Verify SSH connection
    print("[*] Verifying SSH connectivity...")
    test_cmd = [
        "ssh",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=5",
        "-i", str(pub_path.with_suffix("")),
        f"root@{REMOTE_IP}",
        "uname -a; id",
    ]
    try:
        res = subprocess.run(test_cmd, capture_output=True, text=True, check=True)
        print(f"[+] Remote response:\n    {res.stdout.strip()}")
    except subprocess.CalledProcessError as e:
        print(f"[-] SSH verification error: {e.stderr.strip()}")
        return False

    # Back up the remote before the HarmonyNG runtime changes /data
    if not no_backup:
        backup_remote(pub_path.with_suffix(""))

    # 10. Deploy remote payload binaries & rcS.local (from deploy_remote.py)
    print("\n" + "=" * 70)
    print(" [*] DEPLOYING REMOTE RUNTIME & RECOVERY RCS.LOCAL")
    print("=" * 70)
    priv_path = pub_path.with_suffix("")
    ssh_opts = [
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "BatchMode=yes",
        "-i", str(priv_path),
    ]
    scp_opts = ["-O"] + ssh_opts
    target = f"root@{REMOTE_IP}"

    # Upload ARM binaries to /data/
    bin_dir = find_dir_candidate([
        REPO_DIR / "bin",
        REPO_DIR / "remote" / "bin",
        SCRIPT_DIR / "bin",
    ])
    binaries = ["remote_ui", "remote_sync", "scp", "dropbearkey"]
    print("[*] Uploading ARM runtime binaries to /data/...")
    for b in binaries:
        src = bin_dir / b
        if src.is_file():
            print(f"    -> {b}")
            subprocess.run(["scp"] + scp_opts + [str(src), f"{target}:/data/{b}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Upload remote_watchdog.sh supervisor script
    watchdog_src = find_candidate([
        REPO_DIR / "remote" / "remote_watchdog.sh",
        SCRIPT_DIR / "remote_watchdog.sh",
    ])
    if watchdog_src.is_file():
        print(f"[*] Uploading /data/{watchdog_src.name} supervisor script...")
        subprocess.run(["scp"] + scp_opts + [str(watchdog_src), f"{target}:/data/{watchdog_src.name}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    init_src = find_candidate([
        REPO_DIR / "remote" / "init.sh",
        SCRIPT_DIR / "init.sh",
    ])
    if init_src.is_file():
        print(f"[*] Uploading /data/{init_src.name} supervisor script...")
        subprocess.run(["scp"] + scp_opts + [str(init_src), f"{target}:/data/{init_src.name}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Upload rcS.local recovery script
    rcs_file = find_candidate([
        REPO_DIR / "rcS.local",
        REPO_DIR / "remote" / "rcS.local",
        SCRIPT_DIR / "rcS.local",
    ])
    if rcs_file.is_file():
        print("[*] Uploading /etc/init.d/rcS.local (reboot counter & force USB Ethernet)...")
        subprocess.run(["scp"] + scp_opts + [str(rcs_file), f"{target}:/etc/init.d/rcS.local"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if no_screen:
        print("[*] Configuring No-Screen mode in /data/remote_ui.conf...")
        subprocess.run(["ssh"] + ssh_opts + [target, "touch /data/remote_ui.conf; sed -i '/no_screen=/d' /data/remote_ui.conf 2>/dev/null; echo 'no_screen=1' >> /data/remote_ui.conf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Finalize permissions, symlinks, and launch runtime (via watchdog if present)
    print("[*] Finalizing permissions and starting remote supervisor...")
    post_cmds = (
        "sed -i 's/\r$//' /etc/init.d/rcS.local /data/*.sh 2>/dev/null || true; "
        "chmod 755 /data/remote_* /data/init.sh /data/safe_reboot.sh /data/dropbear* /data/scp /etc/init.d/rcS.local 2>/dev/null || true; "
        "chmod -x /usr/bin/usbhid /usr/bin/watchdog 2>/dev/null || true; "
        "ln -sf /data/remote_sync /data/sync 2>/dev/null || true; "
        "ln -sf /data/remote_sync /usr/bin/sync 2>/dev/null || true; "
        "ln -sf /data/dropbearmulti /data/dropbear 2>/dev/null || true; "
        "ln -sf /data/dropbearmulti /usr/sbin/dropbear 2>/dev/null || true; "
        "killall -9 init.sh remote_watchdog.sh remote_ui 2>/dev/null || true; "
        "touch /data/force_sync 2>/dev/null || true; "
        "if [ -x /data/init.sh ]; then /data/init.sh start > /dev/null 2>&1 & elif [ -x /data/remote_watchdog.sh ]; then /data/remote_watchdog.sh > /dev/null 2>&1 & elif [ -x /data/remote_ui ]; then /data/remote_ui > /tmp/remote_ui.log 2>&1 & fi; "
        "sleep 1; pidof remote_ui || echo 'offline'"
    )
    res = subprocess.run(["ssh"] + ssh_opts + [target, post_cmds], capture_output=True, text=True)
    out = res.stdout.strip()
    print(f"[+] Remote status: {out}")
    return True


# ==============================================================================
# Main Orchestrator
# ==============================================================================

def main():
    parser = argparse.ArgumentParser(description="Logitech Harmony remote all-in-one setup tool.")
    parser.add_argument("--step", type=int, choices=[1, 2], help="Run step 1 or 2 individually.")
    parser.add_argument("--key", type=pathlib.Path, default=DEFAULT_KEY_PATH, help="Path to SSH key.")
    parser.add_argument("--skip-fw-check", action="store_true", help="Skip checking for latest official Logitech firmware.")
    parser.add_argument("--check-usb", "--status", dest="check_usb", action="store_true", help="Check and display USB HID / Ethernet / driver state and exit.")
    parser.add_argument("--factory-reset", action="store_true", help="Revert the Harmony remote to official stock firmware.")
    parser.add_argument("--no-screen", action="store_true", help="Install in no-screen mode (for remotes with broken screens).")
    parser.add_argument("--backup", action="store_true", help="Only back up the rooted remote over SSH to backups/remote/ and exit.")
    parser.add_argument("--no-backup", action="store_true", help="Do not back up the remote after SSH comes up.")
    args = parser.parse_args()

    # Pre-execution USB & Network State Detection
    pre_state = detect_remote_usb_state(check_firmware=True)
    print_usb_state_banner(pre_state, label="CURRENT USB DEVICE & NETWORK STATE")

    if args.check_usb:
        sys.exit(0)

    print("=" * 70)
    print(" LOGITECH HARMONY REMOTE (ELITE / ULTIMATE / 950) - ALL-IN-ONE SETUP")
    if args.factory_reset:
        print(" MODE: FACTORY RESET & STOCK RESTORATION")
    if args.no_screen:
        print(" MODE: NO-SCREEN HEADLESS (PHYSICAL BUTTON FORWARDING)")
    print("=" * 70)

    if args.factory_reset:
        conf = input("\nType 'FACTORY-RESET' to confirm reverting the remote to stock: ").strip()
        if conf != "FACTORY-RESET":
            print("[-] Confirmation mismatch. Aborting reset.")
            sys.exit(1)
        if not probe_remote():
            print(f"[-] Cannot reach remote at {REMOTE_IP}:{REMOTE_PORT}. Ensure USB Ethernet is active.")
            sys.exit(1)
        print("[*] Reverting remote to stock configuration...")
        revert_cmds = [
            "if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; fi",
            "killall -9 init.sh remote_watchdog.sh remote_ui 2>/dev/null || true",
            "if [ -f /data/stock_backup/rcS.local.orig ] && ! grep -q 'usbeth' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'dropbear' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'exit 0' /data/stock_backup/rcS.local.orig 2>/dev/null; then cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; else printf '#!/bin/sh\\n/usr/bin/flashuboot /lib/firmware/u-boot.bin 2>/dev/null || true\\n' > /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; fi",
            "chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true",
            "rm -f /etc/nowatchdog /etc/tdeenable /data/reboot_counter /data/remote_ui.conf /tmp/remote_ui.log /tmp/remote-init.log /tmp/remote_stop /tmp/remote_supervisors.pid 2>/dev/null || true",
            "rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true",
            "rm -f /usr/bin/head /usr/bin/tail /usr/bin/which /usr/bin/find /usr/bin/tar /usr/bin/xargs /usr/bin/xz /usr/bin/bzcat /usr/bin/bunzip2 /usr/bin/lzma /usr/bin/unlzma /usr/bin/diff /usr/bin/patch /usr/bin/awk /usr/bin/sed 2>/dev/null || true",
            "/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true",
            "sleep 1",
            "rm -f /data/remote_* /data/init.sh /data/safe_reboot.sh /data/factory_reset.sh /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox 2>/dev/null || true",
            "rm -rf /data/stock_backup /data/assets 2>/dev/null || true",
            "reboot -f 2>/dev/null &",
        ]
        rce("; ".join(revert_cmds))
        print("[+] The remote has been reverted and is rebooting into official stock firmware!")
        sys.exit(0)

    pub_path = ensure_ssh_key(args.key)
    priv_path = pub_path.with_suffix("")
    update_ssh_config(priv_path)

    pub_str = pub_path.read_text(encoding="utf-8").strip()

    if args.backup:
        if not probe_tcp(REMOTE_IP, 22, timeout_s=2.0):
            print(f"[-] No SSH on {REMOTE_IP}:22. Root the remote first (setup_remote.py), then retry.")
            sys.exit(1)
        sys.exit(0 if backup_remote(priv_path) else 1)

    if args.step == 1:
        if pre_state.mode != UsbMode.USB_HID and pre_state.mode != UsbMode.DISCONNECTED:
            print(f"[!] Note: Device currently in {pre_state.mode} mode, Step 1 will attempt USB HID staging.")
        if not step1_enable_usb_debug(pub_str, skip_fw_check=args.skip_fw_check):
            sys.exit(1)
        print(f"[*] Waiting for Dropbear SSH on {REMOTE_IP}:22...")
        if wait_for_ssh(REMOTE_IP, 22, timeout_s=60):
            print(f"\n[+] SUCCESS: Dropbear SSH is ONLINE at {REMOTE_IP}:22!")
            if not args.no_backup:
                backup_remote(priv_path)
            post_state = detect_remote_usb_state(check_firmware=False)
            print_usb_state_banner(post_state, label="POST-INSTALL USB & NETWORK STATE")
            print_success(priv_path)
        else:
            print(f"\n[-] Timed out waiting for Dropbear SSH on {REMOTE_IP}:22.")
            print_step1_instructions()
        return

    if args.step == 2:
        if not probe_remote():
            print(f"[-] Cannot reach {REMOTE_IP}:{REMOTE_PORT}. Ensure USB Ethernet is enabled.")
            if pre_state.remediation_cmd:
                print(f"    Remediation: {pre_state.remediation_cmd}")
            sys.exit(1)
        if not step2_install_permanent_ssh(pub_path, no_screen=args.no_screen, no_backup=args.no_backup):
            sys.exit(1)
        post_state = detect_remote_usb_state(check_firmware=False)
        print_usb_state_banner(post_state, label="POST-INSTALL USB & NETWORK STATE")
        print_success(priv_path)
        return

    # Dynamic Automated Execution based on USB State
    if pre_state.mode == UsbMode.DISCONNECTED:
        print("[-] Remote is not detected on USB bus.")
        print("    Ensure remote is plugged in via direct Micro-USB cable (bottom port) and powered ON.")
        sys.exit(1)

    if pre_state.mode == UsbMode.DRIVER_ERROR:
        print("[-] Windows RNDIS driver error detected.")
        print("    Please follow driver installation instructions above before continuing.")
        sys.exit(1)

    if pre_state.mode == UsbMode.USB_ETHERNET:
        if not pre_state.ip_valid and pre_state.remediation_cmd:
            print(f"[!] Warning: Host network adapter lacks static IP 192.168.2.2.")
            print(f"    Action required: {pre_state.remediation_cmd}")

        if probe_tcp(REMOTE_IP, 22, timeout_s=0.5):
            print(f"[+] Remote is ALREADY online with SSH active at {REMOTE_IP}:22!")
            post_state = detect_remote_usb_state(check_firmware=False)
            print_usb_state_banner(post_state, label="POST-INSTALL USB & NETWORK STATE")
            print_success(priv_path)
            return

        if probe_remote():
            print(f"[+] Remote USB Ethernet active at {REMOTE_IP}:{REMOTE_PORT}. Running Step 2 directly...")
            if not step2_install_permanent_ssh(pub_path, no_screen=args.no_screen, no_backup=args.no_backup):
                print("[-] Step 2 failed.")
                sys.exit(1)
            post_state = detect_remote_usb_state(check_firmware=False)
            print_usb_state_banner(post_state, label="POST-INSTALL USB & NETWORK STATE")
            print_success(priv_path)
            return

    # In USB HID mode -> Stage Dropbear directly over USB HID and reboot
    if not step1_enable_usb_debug(pub_str, skip_fw_check=args.skip_fw_check):
        print("[-] USB HID staging failed. Aborting.")
        sys.exit(1)

    print("[*] Monitoring device reboot and USB Ethernet enumeration...")
    t_end = time.time() + 60
    while time.time() < t_end:
        trans_state = detect_remote_usb_state(check_firmware=False)
        if trans_state.mode in (UsbMode.USB_ETHERNET, UsbMode.DRIVER_ERROR):
            print_usb_state_banner(trans_state, label="DEVICE TRANSITION DETECTED")
            if sys.platform == "win32" and (trans_state.mode == UsbMode.DRIVER_ERROR or not trans_state.ip_valid):
                check_and_fix_windows_driver()
            break
        time.sleep(2)
        print(".", end="", flush=True)

    print(f"\n[*] Waiting for Dropbear SSH on {REMOTE_IP}:22 (up to 60s)...")
    if wait_for_ssh(REMOTE_IP, 22, timeout_s=60):
        print(f"\n[+] SUCCESS: Dropbear SSH is ONLINE at {REMOTE_IP}:22!")
        if not args.no_backup:
            backup_remote(priv_path)
        post_state = detect_remote_usb_state(check_firmware=False)
        print_usb_state_banner(post_state, label="POST-INSTALL USB & NETWORK STATE")
        print_success(priv_path)
        return

    print(f"\n[-] Timed out waiting for SSH. Network adapter may need static IP.")
    print_step1_instructions()
    print("[*] Re-checking for SSH connection (press Ctrl+C to cancel)...")
    if wait_for_ssh(REMOTE_IP, 22, timeout_s=60):
        print(f"\n[+] SUCCESS: Dropbear SSH is ONLINE at {REMOTE_IP}:22!")
        if not args.no_backup:
            backup_remote(priv_path)
        post_state = detect_remote_usb_state(check_firmware=False)
        print_usb_state_banner(post_state, label="POST-INSTALL USB & NETWORK STATE")
        print_success(priv_path)
        return
    sys.exit(1)


def print_success(priv_path: pathlib.Path):
    print("\n" + "=" * 70)
    print(" [+] INSTALLATION COMPLETE! REMOTE IS PERMANENTLY ROOTED")
    print("=" * 70)
    print(" Features enabled:")
    print("  + Hardware watchdog disabled (/etc/nowatchdog)")
    print("  + Dropbear SSH daemon installed on NAND (/data/dropbearmulti)")
    print("  + USB Ethernet & Dropbear auto-start on boot (/etc/init.d/rcS.local)")
    print()
    print(" Connect to your remote anytime over USB:")
    print(f"    ssh -i {priv_path} root@{REMOTE_IP}")
    print(" or simply:")
    print(f"    ssh root@{REMOTE_IP}")
    print()
    print(" Next Step: Install HarmonyNG Custom Firmware & UI:")
    print(f"    python remote/deploy_remote.py --key {priv_path}")
    print("=" * 70 + "\n")



if __name__ == "__main__":
    main()
