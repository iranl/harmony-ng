#!/usr/bin/env python3
"""Load a uImage into Hub RAM over the serial console with U-Boot's `mm`, then optionally boot it.

The stock Hub U-Boot (1.1.4, AP121) has no loady/loadb and the Hub has no Ethernet jack, so the
only way in without writing flash is typing the image word by word into `mm`. `bootm` checks the
uImage data CRC, so a corrupted transfer is refused rather than booted. Nothing is written to flash.

    python tools/uboot_mm_load.py COM6 build/output/kernel6/harmony-hub-initramfs.uImage --catch --boot

--catch sends keys until the `ar7240>` prompt appears: start the tool, then power the Hub on.
"""
import argparse
import sys
import time

import serial

PROMPT = b'ar7240> '
MM_PROMPT = b' ? '


class Console:
    def __init__(self, port, baud, log):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.log = log
        self.buf = b''

    def read(self):
        data = self.ser.read(self.ser.in_waiting or 1)
        if data:
            self.buf += data
            if self.log:
                self.log.write(data)
        return data

    def send(self, text):
        self.ser.write(text.encode() if isinstance(text, str) else text)

    def wait_for(self, token, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if token in self.buf:
                before, _, self.buf = self.buf.partition(token)
                return before
            self.read()
        raise TimeoutError(f'no {token!r} within {timeout} s; last output: {self.buf[-200:]!r}')

    def command(self, line, timeout=10):
        self.buf = b''
        self.send(line + '\r')
        return self.wait_for(PROMPT, timeout)


def catch_prompt(con, timeout):
    print('Power the Hub on now (sending keys to stop autoboot) ...', flush=True)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        con.send(b'\r')
        for _ in range(4):
            con.read()
        if PROMPT in con.buf:
            time.sleep(0.3)
            con.read()
            con.buf = b''
            return
    raise TimeoutError('no U-Boot prompt; is the Hub powered and the port right?')


def load(con, data, addr, lookahead):
    data += b'\0' * (-len(data) % 4)
    words = [int.from_bytes(data[i:i + 4], 'big') for i in range(0, len(data), 4)]
    con.buf = b''
    con.send(f'mm 0x{addr:08x}\r')
    con.wait_for(MM_PROMPT, 5)
    seen = 1                # prompts for words 0 .. seen-1 have appeared
    sent = 0
    start = time.monotonic()
    last_report = start
    while sent < len(words):
        # word k may go out once the prompt for word k - lookahead is on screen
        while sent < len(words) and sent < seen + lookahead:
            con.send(f'{words[sent]:08x}\r')
            sent += 1
        if sent == len(words):
            break
        end = time.monotonic() + 5
        while MM_PROMPT not in con.buf:
            if time.monotonic() > end:
                raise TimeoutError(f'mm stalled at word {sent}: {con.buf[-120:]!r}')
            con.read()
        seen += con.buf.count(MM_PROMPT)
        con.buf = con.buf[con.buf.rfind(MM_PROMPT) + len(MM_PROMPT):]
        now = time.monotonic()
        if now - last_report > 10:
            rate = sent * 4 / (now - start)
            print(f'  {sent * 4}/{len(data)} bytes, {rate / 1024:.1f} KB/s, '
                  f'{(len(data) - sent * 4) / rate:.0f} s left', flush=True)
            last_report = now
    while seen <= len(words):   # wait for the prompt that follows the last word
        con.wait_for(MM_PROMPT, 5)
        seen += 1 + con.buf.count(MM_PROMPT)
        con.buf = con.buf[con.buf.rfind(MM_PROMPT) + len(MM_PROMPT):] if MM_PROMPT in con.buf else con.buf
    con.send('q\r')             # anything that is not hex ends mm
    con.wait_for(PROMPT, 5)
    print(f'Loaded {len(data)} bytes at 0x{addr:08x} in {time.monotonic() - start:.0f} s', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    ap.add_argument('image')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--addr', type=lambda s: int(s, 0), default=0x81000000)
    ap.add_argument('--lookahead', type=int, default=1, help='words sent ahead of the mm prompt (0 = strict)')
    ap.add_argument('--catch', action='store_true', help='stop autoboot first (power the Hub on after start)')
    ap.add_argument('--catch-timeout', type=float, default=120)
    ap.add_argument('--boot', action='store_true', help='bootm the image after loading')
    ap.add_argument('--bootargs', default='console=ttyATH0,115200')
    ap.add_argument('--watch', type=float, default=60, help='seconds of boot output to show')
    ap.add_argument('--log', default='uboot_mm_load.log')
    args = ap.parse_args()

    with open(args.image, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x27\x05\x19\x56':
        sys.exit(f'{args.image} is not a uImage')
    if not 0x80000000 <= args.addr < 0x83800000 or args.addr + len(data) > 0x83800000:
        sys.exit('image would reach U-Boot at the top of RAM (0x83f8b000)')

    with open(args.log, 'wb') as log:
        con = Console(args.port, args.baud, log)
        if args.catch:
            catch_prompt(con, args.catch_timeout)
        else:
            con.command('')
        load(con, data, args.addr, args.lookahead)
        if not args.boot:
            return
        con.command(f'setenv bootargs {args.bootargs}')
        con.buf = b''
        con.send(f'bootm 0x{args.addr:08x}\r')
        end = time.monotonic() + args.watch
        while time.monotonic() < end:
            data = con.read()
            if data:
                sys.stdout.write(data.decode('latin-1'))
                sys.stdout.flush()


if __name__ == '__main__':
    main()
