#!/usr/bin/env python3
"""SSH host key policy shared by the host tools (deploy, setup, backup, restore, reset).

Every tool that runs ssh or scp takes its host key options from ssh_options():

- StrictHostKeyChecking=accept-new: the first key a device shows is recorded, and a later
  connection with a different key is refused (no silent connection to whatever answers at
  the address).
- HostKeyAlias=<host as the tool was given it, lower case> and CheckHostIP=no: the key is
  recorded under exactly that name (no port, no address from an ssh_config HostName, no
  second line for the IP), so --reset-host-key always finds it.
- One project known_hosts file, ~/.ssh/harmony_ng_known_hosts ($HARMONY_KNOWN_HOSTS
  overrides it). The file is per account (Windows and WSL, a user and root each have
  their own). It is kept apart from the user's own known_hosts because every rooted
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
import stat
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


_checked_files: set[str] = set()


def known_hosts_problem() -> str | None:
    """Why the known_hosts file cannot be written, or None. Creates its folder (0700).

    ssh with accept-new that cannot write the file only logs that at INFO level (hidden by
    the LogLevel=ERROR the tools use) and connects anyway: no key would ever be recorded or
    checked. So the tools try what ssh will do: open the file to read and append when it
    exists (it must be a regular file), create the target of a link that points to a file not
    made yet, or create a file in its folder when there is none. A real open also catches, on
    Windows, an access list that denies writing. Whatever the check creates, it removes."""
    path = known_hosts_file()
    try:
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    except OSError as e:
        return f"cannot create {path.parent}: {e.strerror or e}"
    # Read and write (ssh reads the keys too); no blocking on a FIFO
    flags = os.O_RDWR | os.O_APPEND | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0)
    try:
        if path.exists():
            fd = os.open(path, flags)
            try:
                if not stat.S_ISREG(os.fstat(fd).st_mode):
                    return f"{path} is not a regular file"
            finally:
                os.close(fd)
        elif path.is_symlink():
            # A link to a file not created yet: ssh creates the target, so try that and undo it
            fd = os.open(path, flags | os.O_CREAT, 0o600)
            os.close(fd)
            os.unlink(os.path.realpath(path))
        else:
            fd, probe = tempfile.mkstemp(prefix=".known_hosts.", dir=str(path.parent))
            os.close(fd)
            os.unlink(probe)
    except OSError as e:
        return f"cannot write {path}: {e.strerror or e}"
    return None


def require_known_hosts() -> None:
    """Stop the tool when the known_hosts file cannot be written (checked once per file). Every
    tool calls it at the start of main(), before any USB or device work, so the stop always
    comes with nothing changed."""
    key = str(known_hosts_file())
    if key in _checked_files:
        return
    problem = known_hosts_problem()
    if problem:
        sys.exit(f"[-] SSH host keys cannot be recorded: {problem}.\n"
                 f"    Fix the owner or the permissions (for example after a run with sudo), or set "
                 f"{KNOWN_HOSTS_ENV} to a file this account can write. Nothing was changed.")
    _checked_files.add(key)


def entry_name(host: str) -> str:
    """The name the key of host is recorded under (the HostKeyAlias the tools pass)."""
    host = host.rsplit("@", 1)[-1].strip().lower()
    return host


def ssh_options(host: str) -> list[str]:
    """Host key options for ssh and scp (both take -o) to host. Stops the tool when the
    known_hosts file cannot be written (require_known_hosts())."""
    require_known_hosts()
    return [
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "UserKnownHostsFile=" + known_hosts_value(),
        "-o", "HostKeyAlias=" + entry_name(host),
        "-o", "CheckHostIP=no",
        # Plain host names in the file, so forget_host() and the user can find an entry
        "-o", "HashKnownHosts=no",
    ]


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


def _line_matches(line: bytes, names: set[str]) -> bool:
    fields = line.split()
    if not fields or line.lstrip().startswith(b"#") or fields[0].startswith(b"@"):
        return False
    for pattern in fields[0].decode("utf-8", errors="replace").split(","):
        if pattern.lower() in names or any(_hashed_match(pattern, n) for n in names):
            return True
    return False


def forget_host(host: str, port: int = 22, path: pathlib.Path | None = None) -> int:
    """Remove the recorded keys of host (all key types): the entry the tools write (its
    HostKeyAlias) and, for files written otherwise, the host and [host]:port forms. The file's
    other bytes and line ends are kept. Returns how many lines were removed."""
    path = path or known_hosts_file()
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return 0
    name = entry_name(host)
    names = {name, f"[{name}]:{port}"}
    # Split on LF only: a CRLF line keeps its CR (split() treats it as white space)
    lines = data.split(b"\n")
    keep = [ln for ln in lines if not _line_matches(ln, names)]
    removed = len(lines) - len(keep)
    if removed:
        new = b"\n".join(keep)
        try:
            fd, tmp = tempfile.mkstemp(prefix=".known_hosts.", dir=str(path.parent))
        except OSError:
            # A writable file in a folder this account cannot write: rewrite it in place
            with open(path, "r+b") as f:
                f.write(new)
                f.truncate()
            return removed
        try:
            with os.fdopen(fd, "wb") as f:
                f.write(new)
            os.replace(tmp, path)
        except OSError:
            os.unlink(tmp)
            raise
    return removed


def reset_host(host: str, port: int = 22) -> None:
    """--reset-host-key: forget the recorded key and say so."""
    require_known_hosts()
    try:
        removed = forget_host(host, port)
    except OSError as e:
        # Forgetting rewrites the file through a temporary file in its folder
        sys.exit(f"[-] Cannot remove the SSH host key of {host} from {known_hosts_file()}: {e.strerror or e}.\n"
                 f"    Nothing was changed by {RESET_OPTION}.")
    if removed:
        print(f"[*] Removed the recorded SSH host key of {host} from {known_hosts_file()}; "
              "the next connection records the key the device shows now.")
    else:
        print(f"[*] No SSH host key of {host} was recorded in {known_hosts_file()}.")


def is_host_key_failure(stderr: str | bytes | None) -> bool:
    if isinstance(stderr, bytes):
        stderr = stderr.decode("utf-8", errors="replace")
    return bool(stderr) and any(t in stderr for t in _FAILURE_TEXTS)


def host_key_help(host: str, hint: str | None = None) -> str:
    hint = hint or f"run this tool again with {RESET_OPTION} to record it."
    return (f"[-] The SSH host key of {host} is not the one recorded in {known_hosts_file()}.\n"
            f"    A device that was rooted again, factory reset or replaced has a new key:\n"
            f"    {hint}\n"
            f"    Otherwise another device may be answering at {host}: check that before you reset the key.")


def report_failure(stderr: str | bytes | None, host: str, hint: str | None = None) -> bool:
    """When an ssh call failed on the host key, print what to do (to stderr). Returns True
    when it was a host key failure. hint replaces "run this tool again with --reset-host-key"."""
    if not is_host_key_failure(stderr):
        return False
    print(host_key_help(host, hint), file=sys.stderr)
    return True


def add_reset_option(parser: argparse.ArgumentParser, what: str = "the target") -> None:
    parser.add_argument(RESET_OPTION, action="store_true",
                        help=f"Forget the recorded SSH host key of {what} first (after a device was rooted "
                             f"again, factory reset or replaced). Keys are kept in {known_hosts_file()}")
