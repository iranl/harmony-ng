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

    def publish(self, branch: str, version: str, installable: bool | None = None,
                notice_version: str | None = None, url: str | None = None):
        d = self.serve[branch]
        (d / "v2" / "VERSION").write_text(version + "\n")
        if installable is not None:
            v = notice_version or version
            (d / "v2" / "release.json").write_text(json.dumps({
                "version": v, "ota_installable": installable,
                "release_url": url or f"https://github.com/iranl/harmony-ng/releases/tag/v{v}"}))

    def run_sh(self, repo_arg: str, cmd: str, tail: str) -> str:
        r = self.root
        text = SCRIPT.read_text()
        head = text[text.index('OTA_STABLE="'):text.index("TIMESTAMP=")]
        funcs = "".join(function_text(text, n) for n in ("get_local_version", "is_version_newer", "source_version",
                                                          "pick_official_branch", "do_check"))
        body = re.sub(r"(?<![\w/.-])/data/", lambda m: f"{r}{m.group(0)}", head + funcs)
        serve_cases = "\n".join(f'    {url}/*) src="{path}/${{1#{url}/}}" ;;' for url, path in self.serve.items())
        harness = f"""
CMD={cmd}
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
pick_official_branch
{tail}
"""
        out = subprocess.run(["sh", "-c", harness], capture_output=True, text=True, timeout=60)
        self.assertEqual(out.stderr, "")
        return out.stdout

    def check(self, repo_arg: str = "") -> dict:
        stdout = self.run_sh(repo_arg, "check", "do_check")
        line = [ln for ln in stdout.splitlines() if ln.startswith("{")]
        self.assertTrue(line, stdout)
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

    def test_release_notice_of_another_version_or_url_is_ignored(self):
        self.publish(STABLE, "3.5.0", installable=False, notice_version="9.9.9")
        self.assertTrue(self.check()["ota_installable"])
        self.publish(STABLE, "3.5.0", installable=False, url="https://example.org/bundle.zip")
        st = self.check()
        self.assertFalse(st["ota_installable"])
        self.assertEqual(st["release_url"], "https://github.com/iranl/harmony-ng/releases/tag/v3.5.0")

    def test_version_v4_pointer_is_next_major(self):
        self.publish(STABLE, "3.4.0")
        (self.serve[STABLE] / "VERSION_V4").write_text("4.0.0\n")
        st = self.check()
        self.assertFalse(st["update_available"])
        self.assertEqual(st["next_major_version"], "4.0.0")
        self.assertIn("next_major=4.0.0", (self.root / "data/harmony/update_state.conf").read_text())
        # also on beta (the pointer is on ota only)
        (self.root / "data/harmony/update_channel").write_text("beta\n")
        self.publish(BETA, "3.5.0")
        self.assertEqual(self.check()["next_major_version"], "4.0.0")
        (self.serve[STABLE] / "VERSION_V4").write_text("<html>\n")
        self.assertEqual(self.check()["next_major_version"], "")

    def test_beta_follows_the_newer_branch(self):
        (self.root / "data/harmony/update_channel").write_text("beta\n")
        # no ota-beta yet: the stable branch, not the GitHub release list
        self.publish(STABLE, "3.5.0")
        st = self.check()
        self.assertEqual((st["remote_version"], st["channel"]), ("3.5.0", "beta"))
        self.assertIn(f"source={STABLE}", (self.root / "data/harmony/update_state.conf").read_text())
        # a full release newer than the last pre-release
        self.publish(BETA, "3.5.0")
        self.publish(STABLE, "3.5.1")
        self.assertEqual(self.check()["remote_version"], "3.5.1")
        # ota-beta with a v4 pre-release: never followed by a v3 Hub
        self.publish(BETA, "4.0.0")
        st = self.check()
        self.assertEqual((st["remote_version"], st["ota_installable"]), ("3.5.1", True))

    def test_remote_image_from_the_branch_of_the_own_version(self):
        self.publish(STABLE, "3.4.0")
        self.publish(BETA, "3.5.0")
        (self.root / "data/harmony/update_channel").write_text("beta\n")
        self.assertEqual(self.run_sh("", "fetch-remote-ota", 'echo "$REPO"').strip(), STABLE)
        self.publish(STABLE, "3.3.0")
        (self.root / "data/harmony/VERSION").write_text("3.5.0\n")
        (self.root / "data/harmony/update_channel").unlink()
        self.assertEqual(self.run_sh(STABLE, "fetch-remote-ota", 'echo "$REPO"').strip(), BETA)

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

    def test_promotion_refuses_a_damaged_staging(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = pathlib.Path(tmp)
            staging = t / "staging"
            (staging / "bin").mkdir(parents=True)
            (staging / "bin" / "x").write_text("good\n")
            sha = publish_ota.get_file_hashes(staging / "bin" / "x")[1]
            (staging / "manifest.json").write_text(json.dumps(
                {"version": "3.6.0", "files": {"bin/x": {"sha256": sha}}}))
            (staging / "manifest.json.sig").write_text("ab" * 64 + "\n")
            tarball = t / "s.tar.gz"
            (staging / "bin" / "x").write_text("replaced\n")
            with tarfile.open(tarball, "w:gz") as tar:
                for item in staging.iterdir():
                    tar.add(item, arcname=item.name)
            with self.assertRaises(SystemExit):
                publish_ota.promote_staging(tarball, t / "out")

    def test_branch_version_of_a_remote(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = pathlib.Path(tmp)
            src = t / "src"
            (src / "v2").mkdir(parents=True)
            (src / "v2" / "VERSION").write_text("3.9.1\n")
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(["git", "init", "-q", "-b", "ota", str(src)], check=True)
            subprocess.run(git + ["-C", str(src), "add", "."], check=True)
            subprocess.run(git + ["-C", str(src), "commit", "-qm", "v"], check=True)
            self.assertEqual(publish_ota.branch_version(str(src), "ota"), "3.9.1")
            self.assertEqual(publish_ota.branch_version(str(src), "ota-beta"), "")
            self.assertLessEqual(publish_ota.version_tuple("3.9.0"), publish_ota.version_tuple("3.9.1"))


if __name__ == "__main__":
    unittest.main()
