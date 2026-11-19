#!/usr/bin/env python3
"""Set the Hub's Wi-Fi network over USB, when the Hub is on no network.

Connect the Hub's micro-USB port to this computer (the computer powers the Hub), wait about a
minute for it to boot, then:

    python tools/set_wifi_usb.py --status
    python tools/set_wifi_usb.py --ssid "MyNetwork"          # asks for the password
    python tools/set_wifi_usb.py --ssid "Guest" --encryption NONE

A harmony-ng Hub tests the network first and keeps its previous Wi-Fi settings when the test
fails; the reason is shown. Only the Wi-Fi settings can be read and set this way. Stock
(Logitech) firmware answers the same requests. See docs/ETHERNET_AND_NETWORK_SETUP.md.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import sys
import time

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR.parent
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools.harmony_hid import ALL_HUB_PIDS, HarmonyHid, HidErrorResponse, usb_open_failure_message  # noqa: E402

WIFI_PASS_ENV = "HARMONY_WIFI_PASS"
ENCRYPTIONS = ("WPA2-PSK", "WPA-PSK", "NONE")
# Right after Windows set the Hub's USB device up again (a re-enumeration), the Hub's first
# answers can be missing for about a minute; a device opened again then answers.
STATUS_WAIT_S = 60
STATUS_RETRY_S = 5


class UsbAccessError(Exception):
    """The Hub is on USB but could not be opened (Linux: permissions, or the device in use);
    the text says what to do."""


def connect() -> HarmonyHid | None:
    tried: list[HarmonyHid] = []
    for pid in ALL_HUB_PIDS:
        hid = HarmonyHid(pid=pid)
        if hid.open():
            return hid
        tried.append(hid)
    message = usb_open_failure_message(tried)
    if message:
        raise UsbAccessError(message)
    return None


def read_status(hid: HarmonyHid, wait_s: int = STATUS_WAIT_S) -> tuple[HarmonyHid | None, dict[str, str]]:
    """The Hub's Wi-Fi status. When it does not answer, the device is closed and opened again
    every few seconds until wait_s; returns the device in use (None when it is gone from USB at
    the end) and the status ({} if none)."""
    deadline = time.time() + wait_s
    st = hid.get_wifi_status()
    current: HarmonyHid | None = hid
    told = False
    while not st and time.time() < deadline:
        if not told:
            print(f"[*] The Hub does not answer yet (normal for about a minute after the USB device "
                  f"was set up again); retrying for up to {wait_s} s...")
            told = True
        time.sleep(STATUS_RETRY_S)
        if current is not None:
            current.close()
            current = None
        try:
            current = connect()
        except UsbAccessError:
            current = None
        if current is not None:
            st = current.get_wifi_status()
    return current, st


def show_status(st: dict[str, str]) -> None:
    if not st:
        print("[-] The Hub did not answer the Wi-Fi status request.")
        return
    state = st.get("connect_status") or "?"
    ip = st.get("ip_address", "")
    has_ip = bool(ip) and ip != "0.0.0.0"
    network = st.get("ssid") or "(none)"
    if state.lower() == "failed":
        # harmony-ng reports "failed" for 2 minutes after a USB attempt that did not work, while
        # the Hub goes back to its previous network (no name yet while it reconnects)
        if has_ip:
            state = "on its previous network (the last USB attempt failed)"
        elif not st.get("ssid"):
            network = "(reconnecting to the previous network)"
    print(f"    Network:  {network}")
    print(f"    State:    {state}")
    print(f"    Address:  {ip if ip and ip != '0.0.0.0' else '(none)'}")
    if st.get("error_code") and st.get("error_code") != "none":
        print(f"    Last USB attempt failed: {st['error_code']}")
    if st.get("firmware"):
        print(f"    Firmware: {st['firmware']}")


def ask_password(args, ssid: str) -> str:
    if args.encryption == "NONE":
        return ""
    if args.password is not None:
        print(f"[!] --password puts the password in your shell history and the process list; "
              f"leave it out to be asked, or set {WIFI_PASS_ENV}.")
        return args.password
    if os.environ.get(WIFI_PASS_ENV) is not None:
        return os.environ[WIFI_PASS_ENV]
    import getpass
    return getpass.getpass(f"Wi-Fi password for '{ssid}': ")


def is_connected(st: dict[str, str]) -> bool:
    ip = st.get("ip_address", "").strip()
    return st.get("connect_status", "").lower() == "connected" and bool(ip) and ip != "0.0.0.0"


def set_wifi(hid: HarmonyHid, ssid: str, password: str, encryption: str, wait_s: int) -> int:
    print(f"[*] Sending the settings for '{ssid}' ({encryption}); the Hub tests the network first "
          f"(up to about 15 s)...")
    try:
        if not hid.provision_wifi(ssid, password, encryption):
            print("[-] The settings did not reach the Hub (USB write failed). Reconnect the cable and try again.")
            return 1
    except HidErrorResponse as e:
        print(f"[-] {e}")
        print("    This Hub does not offer Wi-Fi setup over USB (older harmony-ng, the setup is turned off in its")
        print("    settings, or the USB port is in host mode).")
        return 1
    except ValueError as e:
        print(f"[-] {e}")
        return 2
    except (RuntimeError, OSError) as e:
        # no answer to the open or the write (timeout), or the device went away
        print(f"[-] The Hub did not answer the request: {e}")
        print("    Check the cable, wait for the Hub to finish starting, and run the tool again.")
        return 1
    deadline = time.time() + wait_s
    st: dict[str, str] = {}
    while True:
        st = hid.get_wifi_status()
        if is_connected(st):
            print(f"[+] Connected to '{st.get('ssid', ssid)}', address {st['ip_address']}.")
            print(f"    The web UI is at http://{st['ip_address']}:8089/")
            return 0
        if st.get("connect_status", "").lower() == "failed":
            print(f"[-] The Hub could not use '{ssid}': {st.get('error_code') or 'failed'}.")
            print("    Its previous Wi-Fi settings are kept. Check the name and password (2.4 GHz networks only).")
            return 1
        if time.time() >= deadline:
            break
        time.sleep(2)
    print(f"[-] No address after {wait_s} s:")
    show_status(st)
    print("    The Hub may still get one: check again with --status.")
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Read or set the Hub's Wi-Fi network over USB (the Hub on this computer's USB port).")
    parser.add_argument("--status", action="store_true", help="only show the Hub's Wi-Fi state")
    parser.add_argument("--ssid", help="Wi-Fi network name (2.4 GHz)")
    parser.add_argument("--password", default=None,
                        help=f"Wi-Fi password (better: leave it out to be asked, or set ${WIFI_PASS_ENV})")
    parser.add_argument("--encryption", default="WPA2-PSK", type=str.upper, choices=ENCRYPTIONS,
                        help="WPA2-PSK (default), WPA-PSK, or NONE for an open network")
    parser.add_argument("--wait", type=int, default=40, metavar="SECONDS",
                        help="how long to wait for an address after the settings were accepted (default 40)")
    args = parser.parse_args(argv)
    if not args.status and not args.ssid:
        parser.error("give --ssid to set the network, or --status")

    try:
        hid = connect()
    except UsbAccessError as e:
        print("[-] " + str(e).replace("\n", "\n    "))
        return 1
    if not hid:
        print("[-] No Harmony Hub found on USB (046D:C129 / 046D:C132).")
        print("    Connect the Hub's micro-USB port to this computer and wait for it to boot. With Ethernet")
        print("    over USB set up, restart the Hub while it is connected to this computer.")
        return 1
    current: HarmonyHid | None = hid
    try:
        print("[*] Hub found on USB. Current Wi-Fi state:")
        current, st = read_status(hid)
        if current is None:
            print("[-] The Hub is no longer on USB (the device went away while waiting for it).")
            return 1
        show_status(st)
        if not st:
            # no answer at all: do not ask for a password that could not be sent
            return 1
        if args.status:
            return 0
        password = ask_password(args, args.ssid)
        return set_wifi(current, args.ssid, password, args.encryption, args.wait)
    finally:
        if current is not None:
            current.close()


if __name__ == "__main__":
    if sys.version_info < (3, 13):
        sys.exit(f"This tool needs Python 3.13 or newer (this is Python {sys.version.split()[0]}).")
    sys.exit(main())
