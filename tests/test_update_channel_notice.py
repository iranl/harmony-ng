#!/usr/bin/env python3
"""Releases that Hubs show but do not install online, and the beta update channel.

payload/scripts/update.sh do_check runs alone in a harness (download_retry serves files from a
temporary "branch" directory, /data moves into a temporary root): another major version and a
release whose release.json says ota_installable false are reported as available but not
installable, with the release page; the beta channel file switches the source to branch
ota-beta, also when the web UI passes the stable URL. scripts/publish_ota.py writes release.json
and promotes a pre-release's staging to the stable channel with the signed manifest unchanged.
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO_DIR / "payload" / "scripts" / "update.sh"
sys.path.insert(0, str(REPO_DIR / "scripts"))
import publish_ota  # noqa: E402

STABLE = "https://raw.githubusercontent.com/iranl/harmony-ng/ota"
BETA = "https://raw.githubusercontent.com/iranl/harmony-ng/ota-beta"


def function_text(text: str, name: str) -> str:
    m = re.search(rf"^{name}\(\) \{{\n.*?^\}}\n", text, re.S | re.M)
    assert m, name
    return m.group(0)


class UpdateCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        r = self.root = pathlib.Path(self.tmp.name)
        (r / "data/harmony").mkdir(parents=True)
        (r / "data/harmony/VERSION").write_text("3.4.0\n")
        (r / "data/harmony/bin").mkdir()
        self.serve = {STABLE: r / "ota", BETA: r / "ota-beta"}
        for d in self.serve.values():
            (d / "v2").mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def publish(self, branch: str, version: str, installable: bool | None = None):
        d = self.serve[branch]
        (d / "v2" / "VERSION").write_text(version + "\n")
        if installable is not None:
            (d / "v2" / "release.json").write_text(json.dumps({
                "version": version, "ota_installable": installable,
                "release_url": f"https://github.com/iranl/harmony-ng/releases/tag/v{version}"}))

    def check(self, repo_arg: str = "") -> dict:
        r = self.root
        text = SCRIPT.read_text()
        head = text[text.index('DEFAULT_OTA="'):text.index("TIMESTAMP=")]
        funcs = "".join(function_text(text, n) for n in ("get_local_version", "is_version_newer", "do_check"))
        body = re.sub(r"(?<![\w/.-])/data/", lambda m: f"{r}{m.group(0)}", head + funcs)
        serve_cases = "\n".join(f'    {url}/*) src="{path}/${{1#{url}/}}" ;;' for url, path in self.serve.items())
        harness = f"""
CMD=check
REPO_ARG='{repo_arg}'
download_retry() {{
  case "$1" in
{serve_cases}
    *) return 1 ;;
  esac
  src="${{src%%\\?*}}"
  [ -f "$src" ] && cp "$src" "$2"
}}
{body}
do_check
"""
        out = subprocess.run(["sh", "-c", harness], capture_output=True, text=True, timeout=60)
        line = [ln for ln in out.stdout.splitlines() if ln.startswith("{")]
        self.assertTrue(line, out.stdout + out.stderr)
        return json.loads(line[-1])

    def test_same_major_installs_online(self):
        self.publish(STABLE, "3.5.0")
        st = self.check()
        self.assertTrue(st["update_available"])
        self.assertTrue(st["ota_installable"])
        self.assertEqual(st["channel"], "stable")

    def test_new_major_is_bundle_only(self):
        self.publish(STABLE, "4.0.0")
        st = self.check()
        self.assertTrue(st["ok"])
        self.assertTrue(st["update_available"])
        self.assertFalse(st["ota_installable"])
        self.assertEqual(st["release_url"], "https://github.com/iranl/harmony-ng/releases/tag/v4.0.0")
        conf = (self.root / "data/harmony/update_state.conf").read_text()
        self.assertIn("ota_installable=0", conf)

    def test_release_notice_makes_it_bundle_only(self):
        self.publish(STABLE, "3.5.0", installable=False)
        st = self.check()
        self.assertTrue(st["update_available"])
        self.assertFalse(st["ota_installable"])

    def test_beta_channel_reads_branch_ota_beta(self):
        self.publish(STABLE, "3.5.0")
        self.publish(BETA, "3.6.0")
        (self.root / "data/harmony/update_channel").write_text("beta\n")
        self.assertEqual(self.check()["remote_version"], "3.6.0")
        # the web UI passes the stable URL by default
        st = self.check(repo_arg=STABLE + "/bin/mips/")
        self.assertEqual(st["remote_version"], "3.6.0")
        self.assertEqual(st["channel"], "beta")

    def test_stable_channel_never_sees_beta(self):
        self.publish(STABLE, "3.4.0")
        self.publish(BETA, "3.6.0")
        st = self.check()
        self.assertEqual(st["remote_version"], "3.4.0")
        self.assertFalse(st["update_available"])


class PublishOta(unittest.TestCase):
    def test_release_info_and_promotion(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = pathlib.Path(tmp)
            staging = t / "staging"
            (staging / "v2").mkdir(parents=True)
            manifest = {"version": "3.6.0", "ota_installable": True, "files": {}}
            (staging / "manifest.json").write_text(json.dumps(manifest))
            (staging / "manifest.json.sig").write_text("ab" * 64 + "\n")
            publish_ota.write_release_info(staging, "3.6.0", True, "beta")
            rel = json.loads((staging / "v2" / "release.json").read_text())
            self.assertEqual((rel["channel"], rel["ota_installable"]), ("beta", True))
            self.assertTrue(rel["release_url"].endswith("/releases/tag/v3.6.0"))
            tarball = t / "ota-staging-v3.6.0.tar.gz"
            with tarfile.open(tarball, "w:gz") as tar:
                for item in staging.iterdir():
                    tar.add(item, arcname=item.name)
            out = t / "promoted"
            self.assertEqual(publish_ota.promote_staging(tarball, out), "3.6.0")
            self.assertEqual(json.loads((out / "release.json").read_text())["channel"], "stable")
            # the signed files are the pre-release's
            self.assertEqual((out / "manifest.json").read_text(), (staging / "manifest.json").read_text())
            self.assertEqual((out / "manifest.json.sig").read_text(), (staging / "manifest.json.sig").read_text())


if __name__ == "__main__":
    unittest.main()
