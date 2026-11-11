#!/usr/bin/env python3
"""
Capture framebuffer screenshots from connected Harmony Elite/Touchscreen remote over SSH.
Dumps /dev/fb0, resolves active panning page (yoffset), converts RGB565 to PNG.
Supports 1x (240x320) and crisp 2x (480x640) scaling for documentation and README.
Zero external dependencies required (uses standard library zlib for PNG encoding).
"""

import argparse
import os
import struct
import subprocess
import sys
import time
import zlib

DEFAULT_REMOTE_IP = "192.168.2.1"
FB_WIDTH = 240
FB_HEIGHT = 320
PAGE_BYTES = FB_WIDTH * FB_HEIGHT * 2  # 153,600 bytes per 240x320 RGB565 page


def write_png_rgb(width, height, rgb_bytes, output_path):
    """Write RGB888 byte array to PNG without PIL dependency."""
    # Each scanline in PNG has a filter byte (0x00 = None) followed by RGB triplets
    raw_scanlines = bytearray()
    row_bytes = width * 3
    for y in range(height):
        raw_scanlines.append(0)  # filter type 0
        raw_scanlines.extend(rgb_bytes[y * row_bytes : (y + 1) * row_bytes])

    compressed_idat = zlib.compress(bytes(raw_scanlines), level=9)

    def png_chunk(chunk_type, data):
        c_len = struct.pack(">I", len(data))
        c_crc = struct.pack(">I", zlib.crc32(chunk_type + data) & 0xFFFFFFFF)
        return c_len + chunk_type + data + c_crc

    header = b"\x89PNG\r\n\x1a\n"
    # IHDR: width (4B), height (4B), bit_depth=8 (1B), color_type=2 (RGB) (1B), compression=0, filter=0, interlace=0
    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    ihdr = png_chunk(b"IHDR", ihdr_data)
    idat = png_chunk(b"IDAT", compressed_idat)
    iend = png_chunk(b"IEND", b"")

    with open(output_path, "wb") as f:
        f.write(header + ihdr + idat + iend)


def rgb565_to_rgb888(raw_565, width=FB_WIDTH, height=FB_HEIGHT, scale=1):
    """Convert RGB565 little-endian raw bytes to RGB888 bytearray with optional integer scaling."""
    out_w = width * scale
    out_h = height * scale
    rgb888 = bytearray(out_w * out_h * 3)

    pixels = struct.unpack(f"<{width * height}H", raw_565[: width * height * 2])

    for y in range(height):
        for x in range(width):
            p = pixels[y * width + x]
            r = ((p >> 11) & 0x1F) * 255 // 31
            g = ((p >> 5) & 0x3F) * 255 // 63
            b = (p & 0x1F) * 255 // 31

            if scale == 1:
                idx = (y * width + x) * 3
                rgb888[idx] = r
                rgb888[idx + 1] = g
                rgb888[idx + 2] = b
            else:
                for dy in range(scale):
                    for dx in range(scale):
                        idx = (((y * scale + dy) * out_w) + (x * scale + dx)) * 3
                        rgb888[idx] = r
                        rgb888[idx + 1] = g
                        rgb888[idx + 2] = b

    return out_w, out_h, bytes(rgb888)


def get_remote_pan(host):
    """Query current framebuffer pan yoffset from sysfs."""
    cmd = [
        "ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=5", f"root@{host}",
        "cat /sys/class/graphics/fb0/pan 2>/dev/null || echo 0,0"
    ]
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        if res.returncode == 0:
            pan_str = res.stdout.decode().strip()
            parts = pan_str.split(",")
            if len(parts) >= 2:
                return int(parts[0]), int(parts[1])
    except Exception:
        pass
    return 0, 0


def capture_framebuffer(host):
    """Fetch raw /dev/fb0 bytes over SSH."""
    cmd = [
        "ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=5", f"root@{host}",
        "cat /dev/fb0"
    ]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    if res.returncode != 0:
        raise RuntimeError(f"SSH failed ({res.returncode}): {res.stderr.decode('utf-8', errors='replace').strip()}")
    return res.stdout


def main():
    parser = argparse.ArgumentParser(description="Capture screenshot from Harmony Elite remote framebuffer.")
    parser.add_argument("--remote", default=DEFAULT_REMOTE_IP, help=f"Remote IP (default: {DEFAULT_REMOTE_IP})")
    parser.add_argument("--output", "-o", default="screenshot.png", help="Output PNG path (default: screenshot.png)")
    parser.add_argument("--scale", "-s", type=int, default=2, choices=[1, 2, 3, 4], help="Image scaling factor (default: 2)")
    parser.add_argument("--page", "-p", type=int, default=-1, help="Force specific page index 0..2 (-1 for auto pan)")
    args = parser.parse_args()

    print(f"[*] Querying framebuffer pan info from root@{args.remote}...")
    xoff, yoff = get_remote_pan(args.remote)
    print(f"[*] Current framebuffer pan: x={xoff}, y={yoff}")

    print(f"[*] Capturing /dev/fb0 stream...")
    fb_bytes = capture_framebuffer(args.remote)
    total_len = len(fb_bytes)
    print(f"[*] Received {total_len} bytes from /dev/fb0 ({total_len // PAGE_BYTES} pages)")

    if total_len < PAGE_BYTES:
        print(f"[-] Error: Incomplete framebuffer read ({total_len} < {PAGE_BYTES} bytes)")
        sys.exit(1)

    if args.page >= 0:
        page_offset = args.page * PAGE_BYTES
    else:
        page_offset = yoff * FB_WIDTH * 2

    if page_offset + PAGE_BYTES > total_len:
        print(f"[!] Warning: Pan offset {page_offset} exceeds total length {total_len}, defaulting to page 0")
        page_offset = 0

    page_bytes = fb_bytes[page_offset : page_offset + PAGE_BYTES]
    w, h, rgb = rgb565_to_rgb888(page_bytes, FB_WIDTH, FB_HEIGHT, scale=args.scale)

    out_dir = os.path.dirname(args.output)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    write_png_rgb(w, h, rgb, args.output)
    print(f"[+] Saved {w}x{h} screenshot to: {args.output}")


if __name__ == "__main__":
    main()
