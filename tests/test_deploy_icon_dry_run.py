#!/usr/bin/env python3
"""build/deploy_hub.py --dry-run in a release-bundle layout with a plain icons.tar.gz: the icon
archive is packed into a temporary folder for its md5, and the dry run names the file the real
run would upload ("Would upload icons.tar.gz to /cache/icons.tar.gz"), not the temporary copy
(it printed "icons.<random>.tar.gz"). The temporary folder is removed and the bundle's archive is
left as it was.

The deployer runs in-process: ssh commands are answered here, nothing else is run (no network).
"""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parent.parent
failed = 0


def check(cond, name, detail=""):
    global failed
    print(("  PASS " if cond else "  FAIL ") + name + ("" if cond else " " + str(detail)))
    if not cond:
        failed += 1


def make_bundle(root: pathlib.Path) -> pathlib.Path:
    b = root / "bundle"
    for sub in ("bin", "www", "scripts", "tools", "modules", "config"):
        (b / sub).mkdir(parents=True)
    shutil.copy(REPO / "build" / "deploy_hub.py", b / "deploy_hub.py")
    for tool in ("ssh_hostkeys.py", "icon_pack.py"):
        shutil.copy(REPO / "tools" / tool, b / "tools" / tool)
    with tarfile.open(b / "icons.tar.gz", "w:gz") as tf:
        for i in range(5):
            data = bytes([i]) * (200 + i)
            info = tarfile.TarInfo(f"icons/i{i}.png")
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    (b / "VERSION").write_text("2.5.3\n")
    (b / "www" / "index.html.gz").write_bytes(b"x")
    return b


def main():
    print("[-] deploy_hub.py --dry-run names the icon archive it would upload")
    with tempfile.TemporaryDirectory() as work:
        work = pathlib.Path(work)
        b = make_bundle(work)
        spare_tmp = work / "tmp"
        spare_tmp.mkdir()
        sys.path.insert(0, str(b / "tools"))
        spec = importlib.util.spec_from_file_location("deploy_hub_bundle", b / "deploy_hub.py")
        dh = importlib.util.module_from_spec(spec)
        sys.modules["deploy_hub_bundle"] = dh
        spec.loader.exec_module(dh)
        for name in dh.REQUIRED_HUB_BINARIES:
            (b / "bin" / name).write_bytes(b"")
        d = dh.HubDeployer(hub_host="hub.invalid", ssh_key="/nonexistent", ui_choice="legacy", no_backup=True, dry_run=True)

        def fake_ssh(cmd):
            if cmd == "uname -m":
                return subprocess.CompletedProcess(cmd, 0, "mips\n", "")
            if cmd.startswith("pidof"):
                return subprocess.CompletedProcess(cmd, 0, "101 102 103", "")
            if "devices.json ] && echo 1" in cmd:
                return subprocess.CompletedProcess(cmd, 0, "1\n", "")
            return subprocess.CompletedProcess(cmd, 0, "", "")

        d.run_ssh = fake_ssh
        before = hashlib.md5((b / "icons.tar.gz").read_bytes()).hexdigest()
        out = io.StringIO()
        with mock.patch.object(dh.subprocess, "run", side_effect=lambda cmd, *a, **k: subprocess.CompletedProcess(cmd, 1, "", "")), \
                mock.patch.object(dh.shutil, "which", return_value=None), \
                mock.patch.object(dh.time, "sleep", lambda s: None), \
                mock.patch.object(tempfile, "tempdir", str(spare_tmp)), \
                mock.patch("builtins.input", return_value="n"), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            try:
                d.deploy()
            except Exception as e:  # noqa: BLE001
                print("EXCEPTION %s: %s" % (type(e).__name__, e))
        text = out.getvalue()
        icon_lines = [ln.strip() for ln in text.splitlines() if "/cache/icons.tar.gz" in ln]
        check(any("would repack icons.tar.gz" in ln for ln in text.splitlines()), "dry run packs the plain archive", text[-2000:])
        check(any(ln.startswith("-> Would upload icons.tar.gz to /cache/icons.tar.gz") for ln in icon_lines),
              "dry run names icons.tar.gz", icon_lines)
        check(not re.search(r"icons\.[A-Za-z0-9_]+\.tar\.gz", text), "no temporary file name in the output",
              re.findall(r"icons\.[A-Za-z0-9_]+\.tar\.gz", text))
        check(os.listdir(spare_tmp) == [], "temporary copy removed", os.listdir(spare_tmp))
        check(hashlib.md5((b / "icons.tar.gz").read_bytes()).hexdigest() == before, "bundle archive unchanged")
    if failed:
        print("[-] test_deploy_icon_dry_run: %d check(s) failed" % failed)
        sys.exit(1)
    print("[+] test_deploy_icon_dry_run: all checks passed")


if __name__ == "__main__":
    main()
