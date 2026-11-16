#!/usr/bin/env python3
"""
The reading end of hub_btstack's key pipe for hub_daemon tests. The mock hardware library
maps /tmp/bthid_input to <mock root>/bthid_input, so a test creates the FIFO there, holds it
open the way hub_btstack does and sees every command hub_daemon hands over, with the time it
arrived (CLOCK_MONOTONIC, in ms).

Used by test_live_key_gaps.py and test_key_trace.py. Pure stdlib.
"""

import os
import select
import threading
import time


class KeyPipeReader:
    """Collects (ms, target, command) for every command line; other lines ("TS ...") are kept
    with the command that follows them. Use as a context manager."""

    def __init__(self, mock_dir):
        self.path = os.path.join(mock_dir, "bthid_input")
        os.mkfifo(self.path)
        # read-write, as hub_btstack opens it: the FIFO stays open without a writer
        self.fd = os.open(self.path, os.O_RDWR | os.O_NONBLOCK)
        self.lock = threading.Lock()
        self.commands = []  # dicts: ms, target, command, extra (list of lines before the command)
        self.reads = []     # raw chunks, one per read()
        self._stop = False
        self._buf = b""
        self._target = ""
        self._extra = []
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def _run(self):
        while not self._stop:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            now = time.monotonic() * 1000.0
            if not chunk:
                continue
            with self.lock:
                self.reads.append(chunk)
                self._buf += chunk
                while b"\n" in self._buf:
                    line, self._buf = self._buf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").strip()
                    if not text:
                        continue
                    if text.startswith("TARGET "):
                        self._target = text[7:]
                    elif text.startswith("TS "):
                        self._extra.append(text)
                    else:
                        self.commands.append({"ms": now, "target": self._target, "command": text,
                                              "extra": self._extra})
                        self._extra = []

    def wait(self, count, timeout=3.0):
        """Waits until `count` commands arrived (or the timeout); returns a copy of the list."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.lock:
                if len(self.commands) >= count:
                    break
            time.sleep(0.005)
        with self.lock:
            return list(self.commands)

    def settle(self, timeout=3.0):
        """hub_daemon sends one RELEASE after its start, as soon as the pipe takes it (it tries
        once a second): waits for it and forgets it, so it does not land between a test's keys."""
        got = self.wait(1, timeout)
        self.clear()
        return [c["command"] for c in got] == ["RELEASE"]

    def clear(self):
        with self.lock:
            self.commands = []
            self.reads = []

    def close(self):
        self._stop = True
        self._thread.join(timeout=1.0)
        try:
            os.close(self.fd)
        except OSError:
            pass
