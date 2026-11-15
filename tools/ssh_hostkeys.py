#!/usr/bin/env python3
"""SSH host key policy shared by the host tools (deploy, setup, backup, restore, reset).

Every tool that runs ssh or scp takes its host key options from ssh_options():

- StrictHostKeyChecking=accept-new: the first key a device shows is recorded, and a later
  connection with a different key is refused (no silent connection to whatever answers at
  the address).
- One project known_hosts file, ~/.ssh/harmony_ng_known_hosts ($HARMONY_KNOWN_HOSTS
  overrides it). It is kept apart from the user's own known_hosts because every rooted
  remote answers at 192.168.2.1 and gets a new key when it is rooted again: resetting that
  entry must not touch anything else. ~/.ssh is where OpenSSH keeps its files on Linux,
  macOS and Windows, and the tools already keep the owner key there.

A device that was rooted again, factory reset or replaced has a new key. The tools then
stop with a message naming --reset-host-key, which removes the recorded key of that
address (forget_host()) so the next connection records the new one.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import os
import pathlib
import sys
import tempfile

KNOWN_HOSTS_ENV = "HARMONY_KNOWN_HOSTS"
RESET_OPTION = "--reset-host-key"

# What OpenSSH prints when the recorded key does not match (or cannot be recorded)
_FAILURE_TEXTS = (
    "REMOTE HOST IDENTIFICATION HAS CHANGED",
    "Host key verification failed",
)


def known_hosts_file() -> pathlib.Path:
    env = os.environ.get(KNOWN_HOSTS_ENV)
    if env:
        return pathlib.Path(env).expanduser()
    return pathlib.Path.home() / ".ssh" / "harmony_ng_known_hosts"


def known_hosts_value() -> str:
    """The known_hosts file as the value of an ssh -o option or an ssh_config line: forward
    slashes on Windows (Windows OpenSSH reads them, and a backslash pair would be read as an
    escape) in double quotes (a path with a space would otherwise be read as two files)."""
    path = known_hosts_file()
    text = path.as_posix() if sys.platform == "win32" else str(path)
    return '"' + text + '"'


def ssh_options() -> list[str]:
    """Host key options for ssh and scp (both take -o)."""
    path = known_hosts_file()
    try:
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    except OSError:
        pass  # ssh reports the problem when it cannot write the file
    return [
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "UserKnownHostsFile=" + known_hosts_value(),
        # Plain host names in the file, so forget_host() and the user can find an entry
        "-o", "HashKnownHosts=no",
    ]


def _entry_name(host: str, port: int = 22) -> str:
    return host if port == 22 else f"[{host}]:{port}"


def _hashed_match(pattern: str, name: str) -> bool:
    """An entry written with HashKnownHosts: |1|base64(salt)|base64(HMAC-SHA1(salt, name))."""
    parts = pattern.split("|")
    if len(parts) != 4 or parts[1] != "1":
        return False
    try:
        salt = base64.b64decode(parts[2])
        digest = base64.b64decode(parts[3])
    except (ValueError, TypeError):
        return False
    return hmac.compare_digest(hmac.new(salt, name.encode(), hashlib.sha1).digest(), digest)


def _line_matches(line: str, name: str) -> bool:
    fields = line.split()
    if not fields or line.lstrip().startswith("#") or fields[0].startswith("@"):
        return False
    for pattern in fields[0].split(","):
        if pattern == name or _hashed_match(pattern, name):
            return True
    return False


def forget_host(host: str, port: int = 22, path: pathlib.Path | None = None) -> int:
    """Remove the recorded keys of host (all key types). Returns how many lines were removed."""
    path = path or known_hosts_file()
    try:
        lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    except FileNotFoundError:
        return 0
    name = _entry_name(host, port)
    keep = [ln for ln in lines if not _line_matches(ln, name)]
    removed = len(lines) - len(keep)
    if removed:
        fd, tmp = tempfile.mkstemp(prefix=".known_hosts.", dir=str(path.parent))
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.writelines(keep)
            os.replace(tmp, path)
        except OSError:
            os.unlink(tmp)
            raise
    return removed


def reset_host(host: str, port: int = 22) -> None:
    """--reset-host-key: forget the recorded key and say so."""
    removed = forget_host(host, port)
    if removed:
        print(f"[*] Removed the recorded SSH host key of {host} from {known_hosts_file()}; "
              "the next connection records the key the device shows now.")
    else:
        print(f"[*] No SSH host key of {host} was recorded in {known_hosts_file()}.")


def is_host_key_failure(stderr: str | bytes | None) -> bool:
    if isinstance(stderr, bytes):
        stderr = stderr.decode("utf-8", errors="replace")
    return bool(stderr) and any(t in stderr for t in _FAILURE_TEXTS)


def host_key_help(host: str) -> str:
    return (f"[-] The SSH host key of {host} is not the one recorded in {known_hosts_file()}.\n"
            f"    A device that was rooted again, factory reset or replaced has a new key: run this tool\n"
            f"    again with {RESET_OPTION} to record it. Otherwise another device may be answering at\n"
            f"    {host}: check that before you reset the key.")


def report_failure(stderr: str | bytes | None, host: str) -> bool:
    """When an ssh call failed on the host key, print what to do (to stderr). Returns True
    when it was a host key failure."""
    if not is_host_key_failure(stderr):
        return False
    print(host_key_help(host), file=sys.stderr)
    return True


def add_reset_option(parser: argparse.ArgumentParser, what: str = "the target") -> None:
    parser.add_argument(RESET_OPTION, action="store_true",
                        help=f"Forget the recorded SSH host key of {what} first (after a device was rooted "
                             f"again, factory reset or replaced). Keys are kept in {known_hosts_file()}")
