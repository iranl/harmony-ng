#!/usr/bin/env python3
"""Cross-platform USB device state, Windows driver, IP diagnostics, and firmware verification.

Supported OS: Windows, Linux, macOS.
"""

from __future__ import annotations

import json
import os
import pathlib
import platform
import re
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Optional, Tuple

REMOTE_IP = "192.168.2.1"
REMOTE_HTTP_PORT = 8088
REMOTE_SSH_PORT = 22

ELITE_PID = 0xC12B
PRO_PID = 0xC132
HARMONY_VID = 0x046D

# RNDIS / Gadget USB IDs
RNDIS_VID_PIDS = [
    (0x0525, 0xA4A2),  # Linux USB Ethernet/RNDIS Gadget
    (0x050D, 0x010D),  # Belkin USB Ethernet Gadget
    (0x046D, 0xC12B),  # Logitech Elite in composite/gadget mode
    (0x046D, 0xC132),  # Logitech Pro in composite/gadget mode
]


class UsbMode:
    USB_HID = "USB_HID"
    USB_ETHERNET = "USB_ETHERNET"
    DRIVER_ERROR = "DRIVER_ERROR"
    DISCONNECTED = "DISCONNECTED"


@dataclass
class UsbState:
    mode: str
    vid: int = HARMONY_VID
    pid: Optional[int] = None
    description: str = ""
    adapter_name: Optional[str] = None
    adapter_desc: Optional[str] = None
    ip_address: Optional[str] = None
    ip_valid: bool = False
    driver_ok: bool = True
    error_code: Optional[int] = None
    error_details: Optional[str] = None
    fw_version: Optional[str] = None
    fw_skin: Optional[int] = None
    fw_latest: Optional[str] = None
    fw_up_to_date: Optional[bool] = None
    http_reachable: bool = False
    ssh_reachable: bool = False
    remediation_cmd: Optional[str] = None


def probe_tcp(ip: str, port: int, timeout_s: float = 0.5) -> bool:
    """Probe if TCP port is open."""
    s = socket.socket()
    s.settimeout(timeout_s)
    try:
        s.connect((ip, port))
        s.close()
        return True
    except Exception:
        return False


def _query_windows_state() -> tuple[Optional[str], Optional[dict], list[dict], list[dict]]:
    """Query Windows PnP devices, network adapters, and IP addresses via PowerShell."""
    ps_script = """
$pnp = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
    $_.FriendlyName -like '*RNDIS*' -or
    $_.FriendlyName -like '*Ethernet Gadget*' -or
    $_.FriendlyName -like '*Composite Gadget*' -or
    $_.InstanceId -like '*VID_046D&PID_C12B*' -or
    $_.InstanceId -like '*VID_046D&PID_C132*' -or
    $_.InstanceId -like '*VID_0525&PID_A4A2*' -or
    $_.InstanceId -like '*VID_050D&PID_010D*'
} | Select-Object FriendlyName, InstanceId, Status, Class, Problem

$adapters = Get-NetAdapter -ErrorAction SilentlyContinue | Select-Object Name, InterfaceDescription, Status, MacAddress
$ips = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object InterfaceAlias, IPAddress, PrefixLength

@{
    Pnp = @($pnp)
    Adapters = @($adapters)
    IPs = @($ips)
} | ConvertTo-Json -Depth 3
"""
    try:
        res = subprocess.run(
            ["powershell", "-NoProfile", "-Command", ps_script],
            capture_output=True,
            text=True,
            timeout=8,
        )
        if res.returncode == 0 and res.stdout.strip():
            data = json.loads(res.stdout)
            pnp = data.get("Pnp", [])
            adapters = data.get("Adapters", [])
            ips = data.get("IPs", [])
            return None, pnp[0] if pnp else None, adapters, ips
    except Exception as e:
        return str(e), None, [], []
    return None, None, [], []


def _query_linux_network() -> tuple[Optional[str], Optional[str], bool]:
    """Query Linux network interfaces for RNDIS/CDC adapters and 192.168.2.x IP."""
    net_dir = pathlib.Path("/sys/class/net")
    if not net_dir.is_dir():
        return None, None, False

    found_if: Optional[str] = None
    found_ip: Optional[str] = None

    for if_path in net_dir.iterdir():
        ifname = if_path.name
        if ifname == "lo":
            continue

        device_driver = if_path / "device" / "driver"
        driver_name = ""
        if device_driver.is_symlink() or device_driver.exists():
            driver_name = device_driver.resolve().name.lower()

        is_rndis = ("rndis" in driver_name or "cdc" in driver_name or
                    ifname.startswith("usb") or ifname.startswith("enx"))

        if is_rndis:
            found_if = ifname
            try:
                res = subprocess.run(["ip", "-j", "addr", "show", ifname], capture_output=True, text=True, timeout=3)
                if res.returncode == 0:
                    data = json.loads(res.stdout)
                    for if_info in data:
                        for addr_info in if_info.get("addr_info", []):
                            if addr_info.get("family") == "inet":
                                ip = addr_info.get("local", "")
                                if ip.startswith("192.168.2."):
                                    return ifname, ip, True
                                elif not found_ip:
                                    found_ip = ip
            except Exception:
                pass

    return found_if, found_ip, (found_ip is not None and found_ip.startswith("192.168.2."))


def _query_macos_network() -> tuple[Optional[str], Optional[str], bool]:
    """Query macOS for RNDIS/Ethernet Gadget service and IP."""
    try:
        res = subprocess.run(["networksetup", "-listallnetworkservices"], capture_output=True, text=True, timeout=3)
        if "RNDIS/Ethernet Gadget" in res.stdout:
            info_res = subprocess.run(
                ["networksetup", "-getinfo", "RNDIS/Ethernet Gadget"],
                capture_output=True,
                text=True,
                timeout=3,
            )
            ip_match = re.search(r"^IP address:\s*([0-9.]+)", info_res.stdout, re.MULTILINE)
            ip = ip_match.group(1) if ip_match else None
            is_valid = bool(ip and ip.startswith("192.168.2."))
            return "RNDIS/Ethernet Gadget", ip, is_valid
    except Exception:
        pass
    return None, None, False


def detect_remote_usb_state(check_firmware: bool = True) -> UsbState:
    """Detect remote connection state across Windows, Linux, and macOS.

    Returns a UsbState object describing USB mode, network, drivers, and firmware.
    """
    state = UsbState(mode=UsbMode.DISCONNECTED)

    # 1. Quick probe for active network reachability (Ethernet mode already running)
    state.http_reachable = probe_tcp(REMOTE_IP, REMOTE_HTTP_PORT, timeout_s=0.3)
    state.ssh_reachable = probe_tcp(REMOTE_IP, REMOTE_SSH_PORT, timeout_s=0.3)

    # 2. Check for USB HID mode
    repo_dir = pathlib.Path(__file__).resolve().parent.parent
    if str(repo_dir) not in sys.path:
        sys.path.insert(0, str(repo_dir))

    hid_instance = None
    try:
        from tools.harmony_hid import HarmonyHid
        for candidate_pid in (ELITE_PID, PRO_PID):
            try:
                h = HarmonyHid(pid=candidate_pid)
                if h.open():
                    hid_instance = h
                    state.mode = UsbMode.USB_HID
                    state.pid = candidate_pid
                    state.description = (
                        "Logitech Harmony Elite 950 (USB HID)"
                        if candidate_pid == ELITE_PID
                        else "Logitech Harmony Pro 2400 (USB HID)"
                    )
                    break
            except Exception:
                continue
    except Exception:
        pass

    # If in USB HID mode, check firmware if requested
    if hid_instance and state.mode == UsbMode.USB_HID:
        if check_firmware:
            try:
                sysinfo = hid_instance.get_sysinfo()
                sw_version = sysinfo.get("sw_version", "").strip()
                skin_str = str(sysinfo.get("skin", "99")).strip()
                skin_id = int(skin_str) if skin_str.isdigit() else 99
                state.fw_version = sw_version if sw_version else None
                state.fw_skin = skin_id

                if state.fw_version:
                    from tools.harmony_firmware import query_latest_firmware
                    latest_res = query_latest_firmware(skin_id)
                    if latest_res and latest_res.get("Id"):
                        state.fw_latest = latest_res["Id"]
                        state.fw_up_to_date = (state.fw_version == state.fw_latest)
            except Exception as e:
                state.error_details = f"Firmware query error: {e}"
        hid_instance.close()
        return state

    # 3. Not in USB HID -> Check USB Ethernet / Drivers per OS
    cur_os = sys.platform

    if cur_os == "win32":
        err, pnp_dev, adapters, ips = _query_windows_state()
        if pnp_dev:
            name = pnp_dev.get("FriendlyName") or pnp_dev.get("InstanceId") or "RNDIS Gadget"
            problem = pnp_dev.get("Problem")

            # Check if driver has failed / missing (Problem != CM_PROB_NONE / Code 28)
            if problem and problem != "CM_PROB_NONE":
                state.mode = UsbMode.DRIVER_ERROR
                state.driver_ok = False
                state.description = name
                state.error_code = 28
                state.error_details = (
                    f"RNDIS hardware detected ({name}), but Windows driver failed to bind "
                    f"(Problem: {problem}). Exclamation mark in Device Manager."
                )
                return state

            # Driver installed properly -> find matching network adapter
            state.mode = UsbMode.USB_ETHERNET
            state.driver_ok = True
            state.description = name

            # Match adapter
            matching_adapter = None
            for ad in adapters:
                ad_name = ad.get("Name", "")
                ad_desc = ad.get("InterfaceDescription", "")
                ad_lower = f"{ad_name} {ad_desc}".lower()
                if "ndis" in ad_lower or "rndis" in ad_lower or "ethernet gadget" in ad_lower:
                    matching_adapter = ad
                    break



            if not matching_adapter and adapters:
                for ad in adapters:
                    ad_name = ad.get("Name", "")
                    ad_desc = ad.get("InterfaceDescription", "")
                    if "usb" in ad_desc.lower() or "gadget" in ad_desc.lower():
                        matching_adapter = ad
                        break

            if matching_adapter:
                state.adapter_name = matching_adapter.get("Name")
                state.adapter_desc = matching_adapter.get("InterfaceDescription")

                # Find IP assigned to this adapter
                for ip_entry in ips:
                    if ip_entry.get("InterfaceAlias") == state.adapter_name:
                        assigned_ip = ip_entry.get("IPAddress", "")
                        state.ip_address = assigned_ip
                        if assigned_ip.startswith("192.168.2."):
                            state.ip_valid = True
                        break

            if not state.ip_valid:
                target_alias = state.adapter_name or "Remote NDIS Compatible Device"
                state.remediation_cmd = (
                    f'New-NetIPAddress -InterfaceAlias "{target_alias}" '
                    f'-IPAddress 192.168.2.2 -PrefixLength 24 -DefaultGateway 192.168.2.1'
                )
            return state

        # If no explicit PnP RNDIS device found, but HTTP/SSH reachable, mark USB Ethernet
        if state.http_reachable or state.ssh_reachable:
            state.mode = UsbMode.USB_ETHERNET
            state.driver_ok = True
            state.ip_valid = True
            state.ip_address = "192.168.2.2"
            state.description = "Active USB Ethernet Gadget"
            return state

    elif cur_os == "darwin":
        if_name, if_ip, ip_ok = _query_macos_network()
        if if_name:
            state.mode = UsbMode.USB_ETHERNET
            state.adapter_name = if_name
            state.ip_address = if_ip
            state.ip_valid = ip_ok
            if not ip_ok:
                state.remediation_cmd = (
                    'sudo networksetup -setmanual "RNDIS/Ethernet Gadget" 192.168.2.2 255.255.255.0'
                )
            return state

        if state.http_reachable or state.ssh_reachable:
            state.mode = UsbMode.USB_ETHERNET
            state.ip_valid = True
            state.ip_address = "192.168.2.2"
            state.description = "Active USB Ethernet Gadget"
            return state

    else:  # Linux / other
        if_name, if_ip, ip_ok = _query_linux_network()
        if if_name:
            state.mode = UsbMode.USB_ETHERNET
            state.adapter_name = if_name
            state.ip_address = if_ip
            state.ip_valid = ip_ok
            if not ip_ok:
                state.remediation_cmd = (
                    f"sudo ip addr add 192.168.2.2/24 dev {if_name} && sudo ip link set {if_name} up"
                )
            return state

        if state.http_reachable or state.ssh_reachable:
            state.mode = UsbMode.USB_ETHERNET
            state.ip_valid = True
            state.ip_address = "192.168.2.2"
            state.description = "Active USB Ethernet Gadget"
            return state

    return state


def print_usb_state_banner(state: UsbState, label: str = "USB DEVICE & NETWORK STATE") -> None:
    """Print structured, clear status banner before/after setup steps."""
    box_w = 70
    print("\n" + "=" * box_w)
    print(f" [{label}]")
    print("=" * box_w)

    os_desc = f"{platform.system()} ({platform.release()})"
    print(f" Host OS:          {os_desc}")

    if state.mode == UsbMode.USB_HID:
        print(f" Connection Mode:  [+] USB HID (Logitech 046D:{state.pid:04X})")
        print(f" Device Model:     {state.description}")
        if state.fw_version:
            skin_name = "Elite 950 / Ultimate" if state.fw_skin == 99 else f"Skin {state.fw_skin}"
            print(f" Hardware Skin:    {skin_name} (ID: {state.fw_skin})")
            print(f" Firmware Version: {state.fw_version}")
            if state.fw_latest:
                status_str = "[+] Up to date" if state.fw_up_to_date else f"[!] OUTDATED (Latest: {state.fw_latest})"
                print(f" Firmware Status:  {status_str}")
        else:
            print(" Firmware Version: [-] NOT RETRIEVED (Device did not return sysinfo)")

    elif state.mode == UsbMode.USB_ETHERNET:
        print(" Connection Mode:  [+] USB Ethernet (RNDIS Gadget)")
        if state.description:
            print(f" Device:           {state.description}")
        if state.adapter_name:
            print(f" Host Adapter:     {state.adapter_name}")
            if state.adapter_desc:
                print(f" Description:      {state.adapter_desc}")
        if state.ip_address:
            ip_status = "[+] Correct (192.168.2.x)" if state.ip_valid else f"[-] INCORRECT (Found {state.ip_address})"
            print(f" Host IPv4:        {state.ip_address} -> {ip_status}")
        else:
            print(" Host IPv4:        [-] Not Assigned (Requires static IP 192.168.2.2)")

        http_str = "[+] Online" if state.http_reachable else "[-] Offline"
        ssh_str = "[+] Online (Port 22)" if state.ssh_reachable else "[-] Offline"
        print(f" Remote HTTP 8088: {http_str}")
        print(f" Remote SSH:       {ssh_str}")

        if not state.ip_valid and state.remediation_cmd:
            print("-" * box_w)
            print(" [!] ACTION REQUIRED: CONFIGURE STATIC IP:")
            print(f"     {state.remediation_cmd}")

    elif state.mode == UsbMode.DRIVER_ERROR:
        print(" Connection Mode:  [-] DRIVER ERROR / MISSING DRIVER")
        print(f" Device:           {state.description}")
        if state.error_details:
            print(f" Details:          {state.error_details}")
        print("-" * box_w)
        print(" [!] ACTION REQUIRED (WINDOWS RNDIS DRIVER):")
        print("     1. Open Device Manager (Win+X -> Device Manager).")
        print("     2. Find device under 'Other devices' with exclamation mark.")
        print("     3. Right-click -> Update driver -> Browse my computer -> Let me pick.")
        print("     4. Select 'Network adapters' -> Manufacturer: 'Microsoft'.")
        print("     5. Model: 'Remote NDIS Compatible Device' -> Click Next.")

    else:
        print(" Connection Mode:  [-] DISCONNECTED")
        print(" Remote is not detected on USB bus.")
        print(" Ensure remote is plugged directly via Micro-USB (not charging dock).")

    print("=" * box_w + "\n")


def require_firmware_or_exit(hid, skin_default: int = 99) -> tuple[str, int]:
    """Retrieve firmware version over USB HID. Hard abort if retrieval fails."""
    try:
        sysinfo = hid.get_sysinfo()
        sw_version = sysinfo.get("sw_version", "").strip()
        skin_str = str(sysinfo.get("skin", str(skin_default))).strip()
        skin_id = int(skin_str) if skin_str.isdigit() else skin_default

        if not sw_version:
            # Fallback direct read attempt
            raw = hid.read_file("/sys/sysinfo")
            if raw:
                text = raw.decode("utf-8", errors="replace")
                for line in text.splitlines():
                    if "sw_version" in line:
                        parts = line.split(",", 1) if "," in line else line.split("=", 1)
                        if len(parts) == 2:
                            sw_version = parts[1].strip()
                            break

        if not sw_version:
            print("\n" + "!" * 70)
            print(" [-] CRITICAL ERROR: FAILED TO RETRIEVE FIRMWARE VERSION OVER USB HID")
            print("!" * 70)
            print(" Remote responded on USB HID, but refused or failed to return /sys/sysinfo.")
            print(" Execution cannot safely continue. Verify cable connection and remote state.")
            print("!" * 70 + "\n")
            sys.exit(1)

        return sw_version, skin_id
    except Exception as e:
        print("\n" + "!" * 70)
        print(f" [-] CRITICAL EXCEPTION: FIRMWARE RETRIEVAL FAILED OVER USB HID: {e}")
        print("!" * 70)
        sys.exit(1)
