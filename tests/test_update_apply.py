#!/usr/bin/env python3
"""payload/scripts/update.sh apply on the host, with a full flash.

The script's /data, /cache, /etc and /tmp paths are moved into a temporary root. wget, tar and the
signature check are fakes that serve a local package; cp fails on one chosen file, or past a byte
budget for /data, to stand in for a full flash. Checks that a failed copy leaves every installed
file whole (old or new, never cut) and VERSION old, with no leftover and no reboot, so a later run
installs everything; that a /data with room for one changed file but not for all of them still
installs; that a web UI copy failure does not record the new version; and that the modular
fallback installs only when every binary the release lists was downloaded.
"""

from __future__ import annotations

import hashlib
import io
import pathlib
import re
import subprocess
import tarfile
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "update.sh"

OLD_VER = "2.4.2"
NEW_VER = "2.9.9"
REPO = "http://pkg.invalid/ota"

# busybox stand-in: wget serves files from $UPD_SERVE, tar is the host's tar
FAKE_BUSYBOX = """#!/bin/sh
case "$1" in
  wget)
    shift
    out=""; url=""
    while [ $# -gt 0 ]; do
      case "$1" in
        -O) out="$2"; shift 2 ;;
        -T|--header) shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
      esac
    done
    rel="${url%%\\?*}"; rel="${rel#*/ota/}"
    [ -f "$UPD_SERVE/$rel" ] || exit 1
    exec /bin/cp "$UPD_SERVE/$rel" "$out" ;;
  tar) shift; exec tar "$@" ;;
  --install) exit 0 ;;
esac
exit 1
"""

# hub_util stand-in: "ota-verify manifest sig" prints the version, with files it accepts them
FAKE_HUB_UTIL = """#!/bin/sh
[ "$1" = "ota-verify" ] || exit 1
if [ $# -eq 3 ]; then
  sed -n 's/.*"version": *"\\([^"]*\\)".*/\\1/p' "$2"
fi
exit 0
"""

# cp that stands in for a full flash: it writes half of the file and fails when the target is
# $UPD_FAIL_CP (with or without .new), or when a copy into $UPD_DATA_DIR would take the files there
# past $UPD_DATA_BUDGET bytes (the target's old content does not count: cp truncates it first)
FAKE_CP = """#!/bin/sh
for last; do :; done
for src; do break; done
name="${last%/}"; name="${name##*/}"; name="${name%.new}"
full=0
if [ -n "$UPD_FAIL_CP" ] && [ "$name" = "$UPD_FAIL_CP" ]; then
  full=1
fi
case "$last" in
  "$UPD_DATA_DIR"/*)
    if [ -n "$UPD_DATA_BUDGET" ] && [ -f "$src" ]; then
      used=$(find "$UPD_DATA_DIR" -type f ! -path "$last" -printf '%s\\n' | awk '{s += $1} END {print s + 0}')
      [ $((used + $(wc -c < "$src"))) -gt "$UPD_DATA_BUDGET" ] && full=1
    fi ;;
esac
if [ "$full" = 1 ]; then
  if [ -f "$src" ]; then
    n=$(( $(wc -c < "$src") / 2 ))
    head -c "$n" "$src" > "$last"
  fi
  echo "cp: write error: No space left on device" >&2
  exit 1
fi
exec /bin/cp "$@"
"""

NOOP = "#!/bin/sh\nexit 0\n"

BINS = ["hub_util", "hub_daemon", "hub_rf", "hub_btstack", "dropbearmulti"]
SCRIPTS = ["init.sh", "rcS.local", "update.sh", "network_manager.sh"]


def md5(path: pathlib.Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()


class UpdateApply(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for d in ("tmp", "data/harmony/bin", "cache/bin", "cache/www/assets", "etc/init.d", "serve/bin/mips", "fakebin"):
            (self.root / d).mkdir(parents=True)
        text = SCRIPT.read_text()
        text = re.sub(r"(?<![\w./])/(data|cache|etc|tmp)/", lambda m: f"{self.root}/{m.group(1)}/", text)
        self.script = self.root / "update.sh"
        self.script.write_text(text)

        fb = self.root / "fakebin"
        for name, body in (("cp", FAKE_CP), ("sleep", NOOP), ("killall", NOOP), ("reboot", NOOP), ("mount", NOOP), ("sync", NOOP)):
            (fb / name).write_text(body)
            (fb / name).chmod(0o755)

        # Installed system: old version of every file
        self.data = self.root / "data" / "harmony"
        self.write(self.data / "VERSION", OLD_VER + "\n")
        self.write(self.root / "cache/bin/busybox", FAKE_BUSYBOX, 0o755)
        self.write(self.data / "bin/hub_util", FAKE_HUB_UTIL, 0o755)
        for b in BINS[1:]:
            self.write(self.data / "bin" / b, f"old {b}\n" * 5000, 0o755)
        for s in SCRIPTS:
            self.write(self.data / s, f"#!/bin/sh\n# old {s}\n", 0o755)
        self.write(self.root / "etc/init.d/rcS.local", "#!/bin/sh\n# old rcS.local\n", 0o755)
        self.write(self.data / "cacert.pem", "old ca\n")
        self.write(self.root / "cache/icons.tar.gz", "old icons\n")
        self.write(self.root / "cache/www/index.html.gz", "old index\n")
        self.write(self.data / "safe_reboot.sh", self.reboot_script(), 0o755)

        # New release in the served directory
        serve = self.root / "serve"
        self.write(serve / "manifest.json", f'{{"version": "{NEW_VER}", "files": {{}}}}\n')
        self.write(serve / "manifest.json.sig", "00" * 64 + "\n")
        self.new_files = {
            "bin/hub_util": FAKE_HUB_UTIL + "# new\n",
            "bin/busybox": FAKE_BUSYBOX + "# new\n",
            "config/cacert.pem": "new ca\n",
            "icons.tar.gz": "new icons\n",
            "www/index.html.gz": "new index\n",
            "www/assets/index-new.js.gz": "new js\n",
            "scripts/safe_reboot.sh": self.reboot_script(),
        }
        for b in BINS[1:]:
            self.new_files[f"bin/{b}"] = f"new {b}\n" * 6000
        for s in SCRIPTS:
            self.new_files[f"scripts/{s}"] = f"#!/bin/sh\n# new {s}\n"

    def tearDown(self):
        self.tmp.cleanup()

    def reboot_script(self) -> str:
        return f"#!/bin/sh\necho rebooted > {self.root}/tmp/rebooted\n"

    @staticmethod
    def write(path: pathlib.Path, text: str, mode: int = 0o644) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(mode)

    def serve_bundle(self) -> None:
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            for name, body in self.new_files.items():
                data = body.encode()
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o755
                tar.addfile(info, io.BytesIO(data))
        (self.root / "serve" / "hub_ota.tar.gz").write_bytes(buf.getvalue())

    def serve_modular(self, bins: list[str], listed: list[str]) -> None:
        files = ", ".join(f'"bin/mips/{b}": {{}}' for b in listed)
        self.write(self.root / "serve" / "manifest.json", f'{{"version": "{NEW_VER}", "files": {{{files}}}}}\n')
        for b in bins:
            self.write(self.root / "serve" / "bin" / "mips" / b, self.new_files[f"bin/{b}"])

    def installed(self) -> dict[str, str]:
        out = {}
        for p in sorted(self.root.rglob("*")):
            rel = p.relative_to(self.root).as_posix()
            if p.is_file() and not rel.startswith(("serve/", "fakebin/", "tmp/")) and rel != "update.sh":
                out[rel] = md5(p)
        return out

    def run_apply(self, fail_cp: str = "", data_budget: int = 0) -> subprocess.CompletedProcess[str]:
        env = {
            "PATH": f"{self.root / 'fakebin'}:/usr/bin:/bin",
            "UPD_SERVE": str(self.root / "serve"),
            "UPD_FAIL_CP": fail_cp,
            "UPD_DATA_DIR": str(self.root / "data"),
            "UPD_DATA_BUDGET": str(data_budget) if data_budget else "",
        }
        return subprocess.run(["sh", str(self.script), "apply", REPO], capture_output=True, text=True, timeout=60, env=env)

    def version(self) -> str:
        return (self.data / "VERSION").read_text().strip()

    def leftovers(self) -> list[str]:
        return sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*.new"))

    def assert_new_installed(self) -> None:
        for b in BINS:
            self.assertEqual((self.data / "bin" / b).read_text(), self.new_files[f"bin/{b}"], b)
        for s in SCRIPTS:
            self.assertEqual((self.data / s).read_text(), self.new_files[f"scripts/{s}"], s)
        self.assertEqual((self.root / "etc/init.d/rcS.local").read_text(), self.new_files["scripts/rcS.local"])
        self.assertEqual((self.root / "cache/bin/busybox").read_text(), self.new_files["bin/busybox"])
        self.assertEqual((self.data / "cacert.pem").read_text(), "new ca\n")
        self.assertEqual((self.root / "cache/icons.tar.gz").read_text(), "new icons\n")

    def test_bundle_installs_everything(self):
        self.serve_bundle()
        r = self.run_apply()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assert_new_installed()
        self.assertEqual((self.root / "cache/www/index.html.gz").read_text(), "new index\n")
        self.assertEqual(self.version(), NEW_VER)
        self.assertTrue((self.root / "tmp" / "rebooted").exists() or "safe Hub reboot" in r.stdout)
        self.assertTrue((self.data / "bin" / "mknod").is_symlink())
        self.assertEqual(self.leftovers(), [])

    def data_used(self) -> int:
        return sum(p.stat().st_size for p in (self.root / "data").rglob("*") if p.is_file())

    def whole_file_pairs(self) -> dict:
        """installed path -> (old content, new content)"""
        pairs = {self.data / "bin" / b: f"bin/{b}" for b in BINS}
        pairs.update({self.data / s: f"scripts/{s}" for s in SCRIPTS})
        pairs[self.root / "cache/bin/busybox"] = "bin/busybox"
        return {p: (p.read_text(), self.new_files[k]) for p, k in pairs.items()}

    def test_failed_copy_leaves_whole_files_and_old_version(self):
        for failing in ("hub_btstack", "dropbearmulti", "update.sh", "busybox"):
            with self.subTest(failing=failing):
                # each case from the old installation
                self.tearDown()
                self.setUp()
                self.serve_bundle()
                pairs = self.whole_file_pairs()
                r = self.run_apply(fail_cp=failing)
                self.assertNotEqual(r.returncode, 0, r.stdout)
                self.assertNotIn("applied successfully", r.stdout)
                self.assertIn("run the update again", r.stdout)
                self.assertEqual(self.version(), OLD_VER)
                for path, (old, new) in pairs.items():
                    self.assertIn(path.read_text(), (old, new), f"{path.name} cut")
                self.assertEqual(self.leftovers(), [])
                self.assertFalse((self.root / "tmp" / "rebooted").exists())
                # Space freed: the same update can be run again and installs everything
                r = self.run_apply()
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assert_new_installed()
                self.assertEqual(self.version(), NEW_VER)

    def test_room_for_one_file_not_all(self):
        """/data has room for one changed file next to the old one, not for all of them"""
        self.serve_bundle()
        # /data files of the package and their installed copies
        targets = {f"bin/{b}": self.data / "bin" / b for b in BINS}
        targets.update({f"scripts/{s}": self.data / s for s in SCRIPTS + ["safe_reboot.sh"]})
        targets["config/cacert.pem"] = self.data / "cacert.pem"
        growth = sum(len(self.new_files[k]) - (p.stat().st_size if p.exists() else 0) for k, p in targets.items())
        largest = max(len(self.new_files[k]) for k in targets)
        # room for the largest new file next to the old ones, after the release is in place
        budget = self.data_used() + growth + largest + 4096
        all_at_once = self.data_used() + sum(len(self.new_files[k]) for k in targets)
        self.assertGreater(all_at_once, budget, "budget must not fit every new file next to the old ones")
        r = self.run_apply(data_budget=budget)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assert_new_installed()
        self.assertEqual(self.version(), NEW_VER)
        self.assertEqual(self.leftovers(), [])

    def test_web_ui_failure_keeps_version(self):
        self.serve_bundle()
        r = self.run_apply(fail_cp="www")
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertEqual(self.version(), OLD_VER)
        self.assertFalse((self.root / "tmp" / "rebooted").exists())
        self.assertEqual(self.leftovers(), [])
        # The retry is not refused as "not newer"
        r = self.run_apply()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.version(), NEW_VER)

    def test_modular_fallback(self):
        listed = ["hub_util", "hub_daemon", "hub_rf", "hub_btstack"]
        before = self.installed()
        # hub_rf listed by the release but not downloadable: nothing installed
        self.serve_modular(["hub_util", "hub_daemon", "hub_btstack"], listed)
        r = self.run_apply()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertEqual(self.installed(), before)
        self.assertEqual(self.version(), OLD_VER)
        # All there, but the flash fills up on hub_rf: stopped there, every file whole, old version
        self.serve_modular(listed, listed)
        r = self.run_apply(fail_cp="hub_rf")
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertEqual((self.data / "bin" / "hub_rf").read_text(), "old hub_rf\n" * 5000)
        self.assertEqual(self.version(), OLD_VER)
        self.assertEqual(self.leftovers(), [])
        # Retry with space: all four installed
        r = self.run_apply()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for b in listed:
            self.assertEqual((self.data / "bin" / b).read_text(), self.new_files[f"bin/{b}"], b)
        self.assertEqual(self.version(), NEW_VER)


if __name__ == "__main__":
    unittest.main(verbosity=2)
