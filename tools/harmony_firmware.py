#!/usr/bin/env python3
"""Logitech Harmony Hub & Elite Remote Firmware & Asset Utility.

Features:
1. Query Logitech SUS servers for latest official firmware versions & URLs.
2. Download official .hfw2 firmware bundles directly from Logitech CloudFront/S3.
3. Extract full Elite UI icon set from firmware squashfs filesystem.
4. Install/flash firmware to connected Hub or Elite Remote over raw USB HID
   without Logitech software or cloud tools.

Usage:
    # Query latest firmware versions from Logitech SUS
    python tools/harmony_firmware.py check

    # Download latest firmware for Hub or Elite
    python tools/harmony_firmware.py download --target hub
    python tools/harmony_firmware.py download --target elite

    # Extract all Elite remote UI icons/assets
    python tools/harmony_firmware.py extract-icons [--firmware <path.hfw2>] [--out-dir data/remote_icons/extracted]

    # Flash firmware to connected Hub or Elite over USB
    python tools/harmony_firmware.py flash --target elite [--firmware <path.hfw2>] [--yes]
    python tools/harmony_firmware.py flash --target hub [--firmware <path.hfw2>] [--yes]
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import pathlib
import shutil
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from typing import Any, Optional

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR.parent

if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools.harmony_hid import (
    HarmonyHid,
    HARMONY_VID,
    HUB_PID,
    HUB_PRO_PID,
    ELITE_PID,
    ALL_HUB_PIDS,
    encode_param_byte,
    encode_param_word_be,
    encode_param_dword_be,
    encode_param_string,
    build_primary_frames,
    build_data_frames,
)

# Logitech Software Update Service (SUS) Endpoint & API Key
SUS_BASE_URL = "https://sus.dhg.myharmony.com/softwareupdatesplatform/softwareupdates/"
SUS_API_KEY = "UV6nr6k8ZqlLYksPWMOlv5qwNU2lo1j8Q57WgwE5"

# Known device skins
SKIN_NAMES = {
    97: "Harmony Hub",
    99: "Harmony Elite / Touch / Ultimate Remote",
    100: "Harmony Home Control / Smart Control Remote",
    102: "Harmony Ultimate Remote",
    105: "Harmony Ultimate Home Remote",
    106: "Harmony Hub (Creemore)",
    108: "Harmony Smart Keyboard Remote",
    111: "Harmony Ultimate One Remote",
    112: "Harmony 950 Remote",
    115: "Harmony Pro Hub (2400)",
    116: "Harmony Pro Remote (2400)",
}

DEFAULT_HUB_SKIN = 97
DEFAULT_ELITE_SKIN = 99


# ==============================================================================
# Logitech SUS Client
# ==============================================================================

def get_ssl_context() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def query_latest_firmware(skin_id: int) -> Optional[dict[str, Any]]:
    """Query Logitech SUS for the latest image update result for a given skin ID."""
    url = f"{SUS_BASE_URL}product/{skin_id}/unit/0/image/latest?trigger=usb&channel=production"
    headers = {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; WOW64)",
        "Logitech-SUS-Key": SUS_API_KEY,
    }
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10, context=get_ssl_context()) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            result = data.get("GetLatestImageUpdateResult")
            if result and result.get("URI"):
                return result
    except urllib.error.HTTPError as e:
        if e.code != 404:
            print(f"[-] HTTP Error {e.code} querying skin {skin_id}: {e.reason}", file=sys.stderr)
    except Exception as e:
        print(f"[-] Error querying skin {skin_id}: {e}", file=sys.stderr)
    return None


def download_file(url: str, dest_path: pathlib.Path, progress_desc: str = "Downloading") -> bool:
    """Download file with live progress indicator."""
    dest_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = dest_path.with_suffix(dest_path.suffix + ".part")

    headers = {"User-Agent": "Mozilla/5.0"}
    req = urllib.request.Request(url, headers=headers)

    try:
        with urllib.request.urlopen(req, timeout=60, context=get_ssl_context()) as resp:
            total_size = int(resp.headers.get("Content-Length", 0))
            downloaded = 0
            block_size = 65536
            start_time = time.time()

            with open(temp_path, "wb") as f:
                while True:
                    chunk = resp.read(block_size)
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)

                    elapsed = max(0.001, time.time() - start_time)
                    rate_kb = (downloaded / 1024.0) / elapsed
                    if total_size > 0:
                        pct = (downloaded / total_size) * 100.0
                        print(
                            f"\r[*] {progress_desc}: {downloaded}/{total_size} bytes ({pct:5.1f}%) "
                            f"[{rate_kb:6.1f} KB/s]",
                            end="",
                            flush=True,
                        )
                    else:
                        print(
                            f"\r[*] {progress_desc}: {downloaded} bytes [{rate_kb:6.1f} KB/s]",
                            end="",
                            flush=True,
                        )
            print()
        if temp_path.is_file():
            if dest_path.is_file():
                dest_path.unlink()
            temp_path.rename(dest_path)
            return True
    except Exception as e:
        if temp_path.is_file():
            temp_path.unlink()
        print(f"\n[-] Download failed: {e}", file=sys.stderr)
        return False
    return False


# ==============================================================================
# Firmware Bundle Parser (.hfw2)
# ==============================================================================

class FirmwareImage:
    def __init__(
        self,
        name: str,
        remote_path: str,
        operation_type: str,
        size: int,
        checksum_type: str,
        checksum_expected: str,
        checksum_length: int,
        checksum_offset: int,
        checksum_seed: int,
        data: bytes,
    ):
        self.name = name
        self.remote_path = remote_path
        self.operation_type = operation_type
        self.size = size
        self.checksum_type = checksum_type
        self.checksum_expected = checksum_expected
        self.checksum_length = checksum_length
        self.checksum_offset = checksum_offset
        self.checksum_seed = checksum_seed
        self.data = data


class FirmwareBundle:
    def __init__(
        self,
        bundle_path: pathlib.Path,
        intended_skins: list[int],
        images: list[FirmwareImage],
        order_elements: list[dict[str, Any]],
    ):
        self.path = bundle_path
        self.intended_skins = intended_skins
        self.images = images
        self.order_elements = order_elements


def parse_hfw2_bundle(bundle_path: pathlib.Path) -> FirmwareBundle:
    """Parse .hfw2 zip archive and Description.xml."""
    if not bundle_path.is_file():
        raise FileNotFoundError(f"Firmware bundle not found: {bundle_path}")

    with zipfile.ZipFile(bundle_path, "r") as z:
        if "Description.xml" not in z.namelist():
            raise ValueError(f"Invalid bundle: Description.xml missing in {bundle_path.name}")

        desc_xml = z.read("Description.xml")
        root = ET.fromstring(desc_xml)

        intended_skins = []
        for skin in root.findall("./INTENDED/SKIN"):
            if skin.text and skin.text.strip():
                try:
                    intended_skins.append(int(skin.text.strip()))
                except ValueError:
                    pass

        files_node = {}
        for f in root.findall("./FILES/FILE"):
            name = (f.get("NAME") or "").strip()
            if name:
                files_node[name] = f

        order_elements = []
        for order in root.findall("./ORDER/ORDER_ELEMENT"):
            order_name = (order.get("NAME") or "").strip()
            reset_flag = (order.get("RESET") or "false").lower() == "true"
            order_elements.append({"name": order_name, "reset": reset_flag})

        images = []
        for elem in order_elements:
            img_name = elem["name"]
            if img_name not in files_node:
                continue
            f_node = files_node[img_name]
            remote_path = (f_node.get("PATH") or "").strip()
            op_type = (f_node.get("OPERATIONTYPE") or "firmwareupgrade").strip()
            data = z.read(img_name)

            cs_node = f_node.find("CHECKSUM")
            if cs_node is not None:
                cs_type = cs_node.get("TYPE", "MD5").strip()
                cs_expected = cs_node.get("EXPECTEDVALUE", "").strip()
                cs_len = int(cs_node.get("LENGTH", len(data)))
                cs_off = int(cs_node.get("OFFSET", 0))
                cs_seed = int(cs_node.get("SEED", 0))
            else:
                cs_type = "MD5"
                cs_expected = hashlib.md5(data).hexdigest()
                cs_len = len(data)
                cs_off = 0
                cs_seed = 0

            images.append(
                FirmwareImage(
                    name=img_name,
                    remote_path=remote_path,
                    operation_type=op_type,
                    size=len(data),
                    checksum_type=cs_type,
                    checksum_expected=cs_expected,
                    checksum_length=cs_len,
                    checksum_offset=cs_off,
                    checksum_seed=cs_seed,
                    data=data,
                )
            )

    return FirmwareBundle(bundle_path, intended_skins, images, order_elements)


# ==============================================================================
# Icon & Asset Extraction
# ==============================================================================

def find_7z_executable() -> Optional[str]:
    """Find 7z executable on system."""
    for name in ("7z", "7zz"):  # Homebrew's sevenzip installs as 7zz
        candidate = shutil.which(name)
        if candidate:
            return candidate
    for default_path in [
        r"C:\Program Files\7-Zip\7z.exe",
        r"C:\Program Files (x86)\7-Zip\7z.exe",
    ]:
        if os.path.isfile(default_path):
            return default_path
    return None


def extract_elite_icons(
    firmware_path: pathlib.Path,
    output_dir: pathlib.Path,
    archive_path: Optional[pathlib.Path] = None,
    android_sync: bool = False,
) -> int:
    """Extract all icons from Elite .hfw2 -> ota-update.EzHex -> squashfs -> opt/luadraws."""
    seven_zip = find_7z_executable()
    if not seven_zip:
        raise RuntimeError(
            "7-Zip is required to unpack squashfs. Install 7-Zip (Windows), p7zip (Linux), "
            "or 'brew install sevenzip' (macOS)."
        )

    print(f"[*] Opening firmware bundle: {firmware_path.name}")
    with zipfile.ZipFile(firmware_path, "r") as z_outer:
        if "ota-update.EzHex" not in z_outer.namelist():
            raise ValueError(f"Bundle {firmware_path.name} does not contain ota-update.EzHex")
        ezhex_data = z_outer.read("ota-update.EzHex")

    print("[*] Extracting inner squashfs filesystem from ota-update.EzHex...")
    with zipfile.ZipFile(io.BytesIO(ezhex_data), "r") as z_inner:
        if "harmony-image.squashfs" not in z_inner.namelist():
            raise ValueError("ota-update.EzHex does not contain harmony-image.squashfs")
        squash_data = z_inner.read("harmony-image.squashfs")

    temp_squash = output_dir / ".harmony-image.squashfs.tmp"
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        temp_squash.write_bytes(squash_data)

        print("[*] Unpacking opt/luadraws/ from squashfs archive...")
        cmd = [
            seven_zip,
            "x",
            f"-o{output_dir}",
            str(temp_squash),
            # Outside Windows, 7-Zip only splits filter paths on "/", so build the filter with the host separator
            os.path.join("opt", "luadraws", "appscommon", "asset", "*"),
            "-r",
            "-y",
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"7z extraction failed: {res.stderr}")

        # Flatten / consolidate icons into a convenient layout
        asset_root = output_dir / "opt" / "luadraws" / "appscommon" / "asset"
        consolidated_icons_dir = output_dir / "icons"
        consolidated_icons_dir.mkdir(parents=True, exist_ok=True)

        extracted_count = 0
        if asset_root.is_dir():
            for png_file in asset_root.rglob("*.png"):
                dest = consolidated_icons_dir / png_file.name
                shutil.copy2(png_file, dest)
                extracted_count += 1

        if extracted_count == 0:
            raise RuntimeError(f"7-Zip extracted no icons from opt/luadraws/appscommon/asset in {firmware_path.name}")
        print(f"[+] Successfully extracted {extracted_count} icons to: {consolidated_icons_dir}")

        if archive_path:
            archive_path.parent.mkdir(parents=True, exist_ok=True)
            print(f"[*] Packaging icons archive into {archive_path}...")
            import tarfile
            with tarfile.open(archive_path, "w:gz") as tar:
                tar.add(consolidated_icons_dir, arcname="icons")
            print(f"[+] Successfully built: {archive_path} ({archive_path.stat().st_size:,} bytes)")

        if android_sync:
            android_icons_dir = REPO_DIR / "android-app" / "public" / "assets" / "icons"
            android_icons_dir.mkdir(parents=True, exist_ok=True)
            copied = 0
            for png_file in consolidated_icons_dir.glob("*.png"):
                shutil.copy2(png_file, android_icons_dir / png_file.name)
                copied += 1
            print(f"[+] Synced {copied} icons directly to Android app assets: {android_icons_dir}")

        return extracted_count
    finally:
        if temp_squash.is_file():
            temp_squash.unlink()


# ==============================================================================
# Firmware USB Flasher (Clean-room Implementation)
# ==============================================================================

def flash_firmware(
    bundle: FirmwareBundle,
    target_device: str = "auto",
    dry_run: bool = False,
    skip_confirm: bool = False,
) -> bool:
    """Flash firmware bundle directly to connected Hub or Elite Remote over USB HID."""
    # 1. Determine target PID
    if target_device.lower() == "hub":
        target_pids = [HUB_PID, HUB_PRO_PID]
    elif target_device.lower() == "elite":
        target_pids = [ELITE_PID]
    else:
        target_pids = [ELITE_PID, HUB_PID, HUB_PRO_PID]

    print("[*] Probing USB HID for connected Harmony device...")
    hid: Optional[HarmonyHid] = None
    connected_pid = None

    for pid in target_pids:
        candidate = HarmonyHid(pid=pid)
        if candidate.open():
            hid = candidate
            connected_pid = pid
            break

    if not hid or not connected_pid:
        print("[-] No compatible Harmony device detected over USB HID.", file=sys.stderr)
        print("    Ensure device is plugged into USB and powered on.", file=sys.stderr)
        return False

    if connected_pid == ELITE_PID:
        dev_name = "Harmony Elite / Touch Remote"
    elif connected_pid == HUB_PRO_PID:
        dev_name = "Harmony Pro Hub (2400)"
    else:
        dev_name = "Harmony Hub"
    print(f"[+] Connected to {dev_name} (USB {HARMONY_VID:04X}:{connected_pid:04X})")

    # 2. Check intended skins
    if bundle.intended_skins:
        print(f"[*] Bundle intended skins: {bundle.intended_skins}")
        if connected_pid in (HUB_PID, HUB_PRO_PID) and not any(s in [97, 106, 109, 115] for s in bundle.intended_skins):
            print(f"[!] Warning: Bundle skins {bundle.intended_skins} do not indicate Hub compatibility!")
        elif connected_pid == ELITE_PID and not any(s in [99, 100, 102, 105, 108, 111, 112, 116] for s in bundle.intended_skins):
            print(f"[!] Warning: Bundle skins {bundle.intended_skins} do not indicate Elite compatibility!")

    # 3. Dry-run or confirmation
    for img in bundle.images:
        print(f"[*] Staged Image: {img.name} -> {img.remote_path} ({img.size} bytes, {img.checksum_type}={img.checksum_expected})")

    if dry_run:
        print("[+] Dry-run requested: Bundle validated, skipping actual flash write.")
        hid.close()
        return True

    if not skip_confirm:
        ans = input(f"\n[?] Flash {bundle.path.name} to {dev_name}? Device will reboot into bootloader updater! (y/N): ")
        if ans.strip().lower() != "y":
            print("[-] Cancelled by user.")
            hid.close()
            return False
            hid.disconnect()
            return False

    try:
        # 4. Reset USB filesystem on device
        print("[*] Resetting device USB filesystem state...")
        if not hid.reset_filesystem():
            print("[-] Reset filesystem returned unexpected response, continuing...")

        # 5. Flash each image
        for img in bundle.images:
            print(f"\n[*] Flashing {img.name} to {img.remote_path} ({img.size} bytes)...")
            handle_id = hid.open_file_write(img.remote_path, size=img.size)
            print(f"[*] Open write succeeded. Allocated handle ID: {handle_id}")

            start_t = time.time()

            def progress_cb(written: int, total: int):
                pct = (written / total) * 100.0 if total > 0 else 0
                elapsed = max(0.001, time.time() - start_t)
                rate = (written / 1024.0) / elapsed
                print(
                    f"\r[*] Writing payload: {written}/{total} bytes ({pct:5.1f}%) [{rate:6.1f} KB/s]",
                    end="",
                    flush=True,
                )

            # Stream chunk burst (burst size 5 packets of 62 bytes each = 310 bytes per burst)
            hid.write_file_chunks(handle_id, img.data, packets_per_chunk=5, progress_callback=progress_cb)
            print()

            # Verify checksum on handle
            print(f"[*] Verifying checksum ({img.checksum_type}: {img.checksum_expected})...")
            cs_ok, cs_result = hid.verify_checksum(
                handle_id=handle_id,
                expected_md5=img.checksum_expected,
                length=img.checksum_length,
                offset=img.checksum_offset,
                seed=img.checksum_seed,
                checksum_type=img.checksum_type,
            )
            print(f"[*] Checksum result: {cs_result} (accepted={cs_ok})")

            # Close handle
            print("[*] Closing file handle...")
            hid.close_file(handle_id)

        # 6. Check if order element requires reset / bootloader handoff
        needs_reset = any(elem.get("reset", False) for elem in bundle.order_elements)
        if needs_reset:
            print("\n[*] Triggering device bootloader flasher and reboot...")
            hid.finalize_reset()
            print("[+] Reset signal sent! Device is now rebooting and executing internal flash updater.")
            print("[+] Keep device connected until LED turns solid green / remote screen boots up.")

        return True
    finally:
        if hid:
            hid.close()


# ==============================================================================
# CLI Entry Point
# ==============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Logitech Harmony Hub & Elite Remote Official Firmware & Icon Utility",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="action", required=True)

    # 1. check
    sub_check = subparsers.add_parser("check", help="Check latest firmware available on Logitech SUS servers")
    sub_check.add_argument("--all-skins", action="store_true", help="Scan all known skins (Hub, Elite, Pro, etc.)")

    # 2. download
    sub_dl = subparsers.add_parser("download", help="Download official firmware bundle from Logitech servers")
    sub_dl.add_argument("--target", choices=["hub", "elite"], default="elite", help="Target device (default: elite)")
    sub_dl.add_argument("--skin", type=int, help="Explicit skin ID to download")
    sub_dl.add_argument("--out-dir", default=str(REPO_DIR / "firmware"), help="Directory to save downloaded bundle")
    sub_dl.add_argument("--extract-ezhex", action="store_true", help="Extract inner ota-update.EzHex after download")

    # 3. extract-icons
    sub_icons = subparsers.add_parser("extract-icons", help="Extract UI icon set from Elite remote firmware")
    sub_icons.add_argument("--firmware", help="Path to local .hfw2 bundle (auto-downloads latest if omitted)")
    sub_icons.add_argument(
        "--out-dir",
        default=str(REPO_DIR / "data" / "remote_icons" / "extracted"),
        help="Output directory for extracted icons",
    )
    sub_icons.add_argument(
        "--archive",
        help="Optional destination path to generate compressed icons.tar.gz archive directly",
    )
    sub_icons.add_argument(
        "--android",
        action="store_true",
        help="Also sync extracted icons directly to android-app/public/assets/icons/",
    )

    # 4. flash / install
    sub_flash = subparsers.add_parser("flash", aliases=["install"], help="Flash firmware bundle directly over USB HID")
    sub_flash.add_argument("--target", choices=["auto", "hub", "elite"], default="auto", help="Target device (default: auto)")
    sub_flash.add_argument("--firmware", help="Path to .hfw2 bundle (auto-downloads latest if omitted)")
    sub_flash.add_argument("--dry-run", action="store_true", help="Validate bundle and intended skins without writing")
    sub_flash.add_argument("--yes", "-y", action="store_true", help="Skip confirmation prompt")

    args = parser.parse_args()

    # ACTION: check
    if args.action == "check":
        skins_to_check = SKIN_NAMES.keys() if args.all_skins else [DEFAULT_HUB_SKIN, DEFAULT_ELITE_SKIN]
        print(f"[*] Querying Logitech SUS Platform for latest firmware:\n")
        print(f"{'Skin':<5} {'Device Name':<42} {'Version':<12} {'Size (B)':<10} {'URL'}")
        print("-" * 110)
        for skin in sorted(skins_to_check):
            name = SKIN_NAMES.get(skin, f"Skin {skin}")
            res = query_latest_firmware(skin)
            if res:
                ver = res.get("Id", "unknown")
                sz = res.get("Size", 0)
                uri = res.get("URI", "")
                print(f"{skin:<5} {name:<42} {ver:<12} {sz:<10} {uri}")
            else:
                print(f"{skin:<5} {name:<42} {'Not Found':<12}")
        print()

    # ACTION: download
    elif args.action == "download":
        skin = args.skin or (DEFAULT_HUB_SKIN if args.target == "hub" else DEFAULT_ELITE_SKIN)
        target_name = SKIN_NAMES.get(skin, f"Skin {skin}")
        print(f"[*] Resolving latest firmware for {target_name} (Skin {skin})...")
        res = query_latest_firmware(skin)
        if not res or not res.get("URI"):
            print(f"[-] Could not retrieve download URL for skin {skin}", file=sys.stderr)
            sys.exit(1)

        url = res["URI"]
        ver = res.get("Id", "latest")
        filename = f"{args.target}_{skin}_{ver}.hfw2"
        out_path = pathlib.Path(args.out_dir) / filename

        print(f"[*] Found firmware version: {ver} ({res.get('Size', 0)} bytes)")
        print(f"[*] Download URL: {url}")
        if not download_file(url, out_path, progress_desc=f"Downloading {filename}"):
            sys.exit(1)

        print(f"[+] Download complete: {out_path}")
        if args.extract_ezhex:
            with zipfile.ZipFile(out_path, "r") as z:
                if "ota-update.EzHex" in z.namelist():
                    ezhex_path = out_path.with_suffix(".EzHex")
                    ezhex_path.write_bytes(z.read("ota-update.EzHex"))
                    print(f"[+] Extracted: {ezhex_path}")

    # ACTION: extract-icons
    elif args.action == "extract-icons":
        fw_path = None
        if args.firmware:
            fw_path = pathlib.Path(args.firmware)
        else:
            # Check local firmware folder first
            firmware_dir = REPO_DIR / "firmware"
            candidates = sorted(firmware_dir.glob("*elite*.hfw2"), key=lambda p: p.stat().st_mtime, reverse=True)
            if candidates:
                fw_path = candidates[0]
                print(f"[*] Using local Elite firmware bundle: {fw_path}")
            else:
                print("[*] No local Elite firmware specified. Downloading latest from Logitech...")
                res = query_latest_firmware(DEFAULT_ELITE_SKIN)
                if not res or not res.get("URI"):
                    print("[-] Failed to query latest Elite firmware URL.", file=sys.stderr)
                    sys.exit(1)
                ver = res.get("Id", "latest")
                fw_path = firmware_dir / f"elite_{DEFAULT_ELITE_SKIN}_{ver}.hfw2"
                if not download_file(res["URI"], fw_path, progress_desc="Downloading Elite firmware"):
                    sys.exit(1)

        out_dir = pathlib.Path(args.out_dir)
        arc_path = pathlib.Path(args.archive) if args.archive else None
        extract_elite_icons(fw_path, out_dir, archive_path=arc_path, android_sync=args.android)

    # ACTION: flash / install
    elif args.action in ["flash", "install"]:
        fw_path = None
        if args.firmware:
            fw_path = pathlib.Path(args.firmware)
        else:
            # Auto-download for detected device
            print("[*] Auto-detecting connected Harmony USB device...")
            connected_pid = None
            for pid in [ELITE_PID, HUB_PID, HUB_PRO_PID]:
                test_hid = HarmonyHid(pid=pid)
                if test_hid.open():
                    connected_pid = pid
                    test_hid.close()
                    break

            if not connected_pid:
                print("[-] No Harmony device detected over USB. Specify --firmware manually or connect device.", file=sys.stderr)
                sys.exit(1)

            if connected_pid == ELITE_PID:
                target = "elite"
                skin = DEFAULT_ELITE_SKIN
            elif connected_pid == HUB_PRO_PID:
                target = "hub"
                skin = 115
            else:
                target = "hub"
                skin = DEFAULT_HUB_SKIN
            print(f"[+] Detected {target.upper()} (Skin {skin}). Downloading latest official firmware...")
            res = query_latest_firmware(skin)
            if not res or not res.get("URI"):
                print("[-] Failed to query latest firmware URL.", file=sys.stderr)
                sys.exit(1)
            ver = res.get("Id", "latest")
            firmware_dir = REPO_DIR / "firmware"
            fw_path = firmware_dir / f"{target}_{skin}_{ver}.hfw2"
            if not fw_path.is_file():
                if not download_file(res["URI"], fw_path, progress_desc=f"Downloading {fw_path.name}"):
                    sys.exit(1)
            else:
                print(f"[*] Using cached firmware: {fw_path}")

        bundle = parse_hfw2_bundle(fw_path)
        ok = flash_firmware(
            bundle=bundle,
            target_device=args.target,
            dry_run=args.dry_run,
            skip_confirm=args.yes,
        )
        if not ok:
            sys.exit(1)


if __name__ == "__main__":
    main()
