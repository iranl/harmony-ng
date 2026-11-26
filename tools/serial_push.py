#!/usr/bin/env python3
"""Copy a file into a Linux shell on a serial console (BusyBox base64 -d), checked by MD5.

For a Hub that runs a test kernel from RAM with no network yet:

    python tools/serial_push.py COM6 ath9k.ko /tmp/ath9k.ko

The shell must be at its prompt. Echo is turned off during the transfer and back on after.
"""
import argparse
import base64
import hashlib
import sys
import time

import serial

BAUD_BYTES = 11520  # 115200 baud, 8N1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    ap.add_argument('src')
    ap.add_argument('dest')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--rate', type=float, default=0.5,
                    help='share of the line rate to send at (0.9 loses bytes on the Hub UART)')
    args = ap.parse_args()

    data = open(args.src, 'rb').read()
    lines = base64.encodebytes(data).splitlines(keepends=True)
    ser = serial.Serial(args.port, args.baud, timeout=0)

    def shell(cmd, wait):
        ser.reset_input_buffer()
        ser.write(cmd.encode() + b'\n')
        time.sleep(wait)
        return ser.read(65536).decode('latin-1')

    shell('stty -echo', 1)
    ser.write(f'base64 -d > {args.dest}\n'.encode())
    time.sleep(0.5)
    start = time.monotonic()
    sent = 0
    for i in range(0, len(lines), 16):
        chunk = b''.join(lines[i:i + 16])
        ser.write(chunk)
        sent += len(chunk)
        # the console has no flow control: pace below the line rate
        ahead = sent / (BAUD_BYTES * args.rate) - (time.monotonic() - start)
        if ahead > 0:
            time.sleep(ahead)
        ser.reset_input_buffer()
    ser.write(b'\x04')
    time.sleep(1)
    out = shell(f'stty echo; md5sum {args.dest}', 3)
    want = hashlib.md5(data).hexdigest()
    print(f'{len(data)} bytes in {time.monotonic() - start:.0f} s')
    if want not in out:
        sys.exit(f'MD5 mismatch: want {want}, Hub said {out.strip()!r}')
    print(f'{args.dest}: MD5 {want} OK')


if __name__ == '__main__':
    main()
