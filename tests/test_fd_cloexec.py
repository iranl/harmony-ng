#!/usr/bin/env python3
"""Long-lived file descriptors are close-on-exec.

hub_daemon, hub_rf and remote_ui start helpers with system() (dropbear, network scripts, a
restarted daemon). A helper inherits every fd that is not close-on-exec and keeps it for its whole
life: the single-user /dev/rfspi, the single-instance locks (the next start then finds the lock
taken), multicast sockets nobody reads, an MQTT connection that never closes, /dev/i2s.

Each open() or socket() listed here must either pass O_CLOEXEC or set FD_CLOEXEC within the
next six lines. Every pattern must still be found, so a moved call is noticed.
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "payload" / "source"

SITES = [
    ("remote/remote_main.c", r'open\("/var/run/remote_ui\.lock"'),
    ("remote/remote_main.c", r"open\(FB_DEV,"),
    ("remote/remote_main.c", r"open\(GYRO_EVENT_DEV,"),
    ("remote/remote_main.c", r"open\(RFSPI_DEV,"),
    ("remote/remote_main.c", r"open\(BTN_EVENT_DEV,"),
    ("remote/remote_input.c", r"open\(candidates\[i\],"),
    ("remote/remote_input.c", r"open\(TOUCH_EVENT_DEV,"),
    ("ir_i2s.c", r"open\(I2S_LOCK_FILE,"),
    ("ir_i2s.c", r"open\(I2S_DEVICE,"),
    ("mqtt_client.c", r"socket\(AF_INET, SOCK_STREAM"),
    ("hub_mdns.c", r"socket\(AF_INET, SOCK_DGRAM"),
    ("hub_ssdp.c", r"socket\(AF_INET, SOCK_DGRAM"),
    ("rf_spi.c", r"open\(RFSPI_DEV,"),
    ("rf_main.c", r'open\("/var/run/hub_rf\.lock"'),
    ("wifi_config.c", r"open\(path, O_CREAT \| O_RDWR"),
]


def main() -> int:
    failures = []
    for rel, pattern in SITES:
        lines = (SRC / rel).read_text().splitlines()
        hits = [i for i, line in enumerate(lines) if re.search(pattern, line)]
        if not hits:
            failures.append(f"{rel}: no call matches {pattern}")
            continue
        for i in hits:
            window = lines[i:i + 7]
            if "CLOEXEC" in lines[i] or any("FD_CLOEXEC" in w for w in window[1:]):
                continue
            failures.append(f"{rel}:{i + 1}: not close-on-exec: {lines[i].strip()}")
    for f in failures:
        print(f"[-] {f}")
    if failures:
        print(f"[-] test_fd_cloexec: {len(failures)} fd(s) a helper would inherit")
        return 1
    print(f"[+] test_fd_cloexec: {len(SITES)} kinds of long-lived fds are close-on-exec")
    return 0


if __name__ == "__main__":
    sys.exit(main())
