"""hub_btstack's status file for the tests, from the real writer.

tests/bt_status_sample.c runs payload/source/hub_btstack/bt_status_json.c (the code
update_status() in bt_pairing.c uses) with dummy addresses; this module builds it once and
writes its output where hub_daemon reads it. Never write a hand-made status object: the
documented shape and the daemon's drifted apart once (top-level running/connected, target =
host), and the fixtures followed the hand-made one.
"""
import os
import subprocess

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TESTS_DIR)
BT_STATUS_FILE = "/tmp/hub_btstack_status.json"
_BIN = os.path.join(REPO_ROOT, "build", "tmp", "test_host", "bt_status_sample")
_SRCS = [os.path.join(TESTS_DIR, "bt_status_sample.c"),
         os.path.join(REPO_ROOT, "payload", "source", "hub_btstack", "bt_status_json.c")]


def _build():
    if os.path.exists(_BIN) and all(os.path.getmtime(_BIN) >= os.path.getmtime(s) for s in _SRCS):
        return
    os.makedirs(os.path.dirname(_BIN), exist_ok=True)
    subprocess.run(["gcc", "-Wall", "-Wextra", "-O2",
                    "-I" + os.path.join(REPO_ROOT, "payload", "source", "hub_btstack"),
                    "-o", _BIN] + _SRCS, check=True)


def sample_text(pid=None, state=None, host_connected=True, pairing=False, no_hosts=False, host_name=None,
                last_button=None, last_seen=None):
    """The file's text as hub_btstack writes it"""
    _build()
    args = [_BIN, "--pid", str(os.getpid() if pid is None else pid)]
    if state:
        args += ["--state", state]
    if not host_connected:
        args += ["--host-connected", "0"]
    if pairing:
        args += ["--pairing", "1"]
    if no_hosts:
        args.append("--no-hosts")
    if host_name is not None:
        args += ["--host-name", host_name]
    if last_button is not None:
        args += ["--last-button", last_button]
    if last_seen is not None:
        args += ["--last-seen", str(last_seen)]
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def write_status(path=BT_STATUS_FILE, **kw):
    """Writes the file atomically, as hub_daemon would find it"""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(sample_text(**kw))
    os.replace(tmp, path)


def remove_status(path=BT_STATUS_FILE):
    for p in (path, path + ".tmp"):
        try:
            os.remove(p)
        except OSError:
            pass
