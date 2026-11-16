#!/usr/bin/env python3
"""tools/sync_remote_to_hub.py: import flag, --force, --skip-icons, failed transfers.

The tool runs as a subprocess with a fake ssh first in PATH (no network). The fake tells
the two devices apart by the login (root@remote.invalid, root@hub.invalid), logs every
command, answers the probes, and runs the Hub's icon folder command in a scratch folder
that stands for the Hub's /data and /cache.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
TOOL = REPO_DIR / "tools" / "sync_remote_to_hub.py"

FAKE_SSH = r"""#!/bin/sh
for last; do :; done
for a; do case "$a" in root@*) host=${a#root@} ;; esac; done
printf '%s|%s\n' "$host" "$last" >> "$FAKE_DIR/log"
case "$last" in
  "uname -m") [ "$host" = hub.invalid ] && echo "$FAKE_HUB_MACHINE" || echo armv5tejl; exit 0 ;;
  *".imported ] && echo 'imported'"*) echo "$FAKE_FLAG"; exit 0 ;;
  *"ln -sfn /cache/www/assets/icons"*)
    sh -c "$(printf '%s' "$last" | sed "s#/data/#$FAKE_ROOT/data/#g; s#/cache/#$FAKE_ROOT/cache/#g")"; exit $? ;;
  *"tar -xzf - -C /cache/www/assets/icons"*) cat >/dev/null; exit "${FAKE_UNTAR_RC:-0}" ;;
  *"tar -xzf -"*) cat >/dev/null; exit 0 ;;
  *"tar -c"*) printf 'x'; exit 0 ;;
  *"remote_ui.conf 2>/dev/null"*) exit 0 ;;
  *"ActivityList.json ] && echo"*) echo empty; exit 0 ;;
  *) exit 0 ;;
esac
"""


class SyncRemoteToHub(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        (base / "bin").mkdir()
        ssh = base / "bin" / "ssh"
        ssh.write_text(FAKE_SSH)
        ssh.chmod(0o755)
        self.root = base / "hub"
        (self.root / "cache" / "www" / "assets" / "icons").mkdir(parents=True)
        (self.root / "data" / "harmony").mkdir(parents=True)
        self.base = base
        self.env = dict(os.environ, PATH=f"{base / 'bin'}:{os.environ['PATH']}", FAKE_DIR=str(base),
                        FAKE_ROOT=str(self.root), FAKE_HUB_MACHINE="mips", FAKE_FLAG="not_imported")

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args, **env) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(TOOL), "--remote", "remote.invalid", "--hub", "hub.invalid", *args],
                              env=dict(self.env, **env), capture_output=True, text=True, timeout=60)

    def log(self) -> list[str]:
        p = self.base / "log"
        return p.read_text().splitlines() if p.exists() else []

    def test_not_imported_is_not_reported_as_imported(self):
        res = self.run_tool("--skip-config")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("NOT YET IMPORTED", res.stdout)

    def test_imported_needs_force(self):
        res = self.run_tool(FAKE_FLAG="imported")
        self.assertEqual(res.returncode, 0, res.stdout)
        self.assertFalse([c for c in self.log() if "tar" in c], "imported again without --force")
        res = self.run_tool("--force", "--skip-config", FAKE_FLAG="imported")
        self.assertTrue([c for c in self.log() if "tar -c" in c], "--force was ignored")

    def test_skip_icons_leaves_the_icon_folders(self):
        self.run_tool("--skip-icons", "--skip-config")
        self.assertFalse([c for c in self.log() if "assets/icons" in c], "--skip-icons still changed the icon folders")

    def test_linked_www_gets_no_self_link(self):
        (self.root / "data" / "harmony" / "www").symlink_to(self.root / "cache" / "www")
        (self.root / "cache" / "www" / "assets" / "icons" / "keep.png").write_bytes(b"png")
        res = self.run_tool("--skip-config")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        icons = self.root / "cache" / "www" / "assets" / "icons"
        self.assertFalse((icons / "icons").is_symlink(), "icons/icons link created inside the icon folder")
        self.assertTrue((icons / "keep.png").exists(), "the icon folder was emptied through the link")

    def test_failed_icon_stream_is_a_failure(self):
        res = self.run_tool("--skip-config", FAKE_UNTAR_RC="1")
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertNotIn("sync complete", res.stdout)

    def test_refuses_a_hub_that_is_not_one(self):
        res = self.run_tool(FAKE_HUB_MACHINE="armv5tejl")
        self.assertNotEqual(res.returncode, 0)
        self.assertEqual([c for c in self.log() if c.startswith("hub.invalid|") and c != "hub.invalid|uname -m"], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
