#!/usr/bin/env python3
"""payload/scripts/update.sh and init.sh on the host, paths moved into a temporary directory:

- a download that fails the certificate check is fetched again without it only when the release
  signature covers the file (repair-tls: manifest.json), and then without the token; the
  version check (VERSION files, the GitHub release list), which nothing signs, is not fetched
  again that way and never sends the token to an unchecked server;
- the log line names the whole reason hub_fetch gave (it was cut at the last "TLS");
- the token goes only to the configured source; the version check's fallback to the GitHub
  release list is asked only for the GitHub repository the source is on (the default source on
  this code) and never with the token (with a custom source and a token, the token went to api.github.com when
  the source's VERSION files could not be fetched), with hub_fetch and with busybox wget;
- a lock directory whose pid file is not written yet belongs to a run that is starting: a second
  run waits for the pid file and stays out (it took the lock over at once, so both ran);
- the init.sh loop that repairs hub_fetch and libbearssl.so.0 keeps /tmp/update_repair.log at
  the first 6 tries, then only the latest one (it grew by every try, once an hour, for ever).
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import time
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
UPDATE = REPO_DIR / "payload" / "scripts" / "update.sh"
INIT = REPO_DIR / "payload" / "scripts" / "init.sh"
TLS_REASON = "TLS: server needs an unsupported TLS version (TLS 1.2 is needed to check its certificate)"


class Sandbox(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for d in ("tmp", "data/harmony/bin", "cache", "stub"):
            (self.root / d).mkdir(parents=True)
        self.script = self.root / "update.sh"
        self.script.write_text(self.moved(UPDATE.read_text()))
        stub = self.root / "stub" / "busybox"
        stub.write_text('#!/bin/sh\nif [ "$1" = wget ]; then exit 1; fi\nexec "$@"\n')
        stub.chmod(0o755)
        (self.root / "uptime").write_text("1234.56 99.0\n")
        self.env = dict(os.environ, PATH=f"{self.root}/stub:/usr/bin:/bin", UPDATE_UPTIME_FILE=str(self.root / "uptime"))
        self.holder = None

    def tearDown(self):
        if self.holder and self.holder.poll() is None:
            self.holder.kill()
            self.holder.wait()
        self.tmp.cleanup()

    def moved(self, text: str) -> str:
        for p in ("/tmp/", "/data/", "/cache/", "/etc/"):
            text = text.replace(p, f"{self.root}{p}")
        return text

    def run_update(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["sh", str(self.script), *args], env=self.env, capture_output=True, text=True, timeout=120)


class TlsRetry(Sandbox):
    """hub_fetch stub: with --ca it fails the certificate check; with --insecure it answers. Every
    call is logged with its arguments and the header file it was given."""

    def setUp(self):
        super().setUp()
        self.calls = self.root / "fetch.log"
        hub_fetch = self.root / "data" / "harmony" / "bin" / "hub_fetch"
        hub_fetch.write_text(
            "#!/bin/sh\n"
            '[ "$1" = --probe ] && exit 0\n'
            'hdr=""; out=""; url=""; mode=""\n'
            "while [ $# -gt 0 ]; do\n"
            '  case "$1" in\n'
            '    --header-file) hdr="$2"; shift ;;\n'
            '    -o) out="$2"; shift ;;\n'
            '    --ca) mode=ca; shift ;;\n'
            '    --insecure) mode=insecure ;;\n'
            '    -T|--max) shift ;;\n'
            '    *) url="$1" ;;\n'
            "  esac\n"
            "  shift\n"
            "done\n"
            f'echo "$mode $url header=$([ -n "$hdr" ] && cat "$hdr")" >> \'{self.calls}\'\n'
            'if [ "$mode" = ca ]; then\n'
            f'  echo "hub_fetch: $url: {TLS_REASON}" >&2; exit 1\n'
            "fi\n"
            'echo "1.2.3" > "$out"; exit 0\n')
        hub_fetch.chmod(0o755)
        (self.root / "data" / "harmony" / "cacert.pem").write_text("x\n")

    def lines(self) -> list[str]:
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def test_version_check_not_fetched_without_the_check(self):
        res = self.run_update("check", "https://example.invalid/ota", "tok123")
        calls = self.lines()
        self.assertTrue(any(c.startswith("ca ") for c in calls), calls)
        self.assertFalse([c for c in calls if c.startswith("insecure ")], "nothing unsigned is fetched unchecked")
        self.assertIn("no signature covers this file", res.stderr)

    def test_signed_file_fetched_again_without_the_token(self):
        res = self.run_update("repair-tls", "https://example.invalid/ota", "tok123")
        calls = self.lines()
        ca = [c for c in calls if c.startswith("ca ") and "manifest.json?" in c]
        insecure = [c for c in calls if c.startswith("insecure ") and "manifest.json?" in c]
        self.assertTrue(ca and "tok123" in ca[0], "the checked server gets the token: %s" % calls)
        self.assertTrue(insecure, "the signed manifest is fetched again without the check: %s" % calls)
        self.assertFalse([c for c in insecure if "tok123" in c], "never the token without the check: %s" % insecure)
        self.assertIn(TLS_REASON + ". Fetched again without the certificate check", res.stderr)


class GithubFallback(Sandbox):
    """Every download fails, so the version check reaches its fallback. The downloader stub logs
    each URL with the token it was given (hub_fetch: header file; wget: --header)."""

    def setUp(self):
        super().setUp()
        self.calls = self.root / "fetch.log"

    def use_hub_fetch(self):
        hub_fetch = self.root / "data" / "harmony" / "bin" / "hub_fetch"
        hub_fetch.write_text(
            "#!/bin/sh\n"
            '[ "$1" = --probe ] && exit 0\n'
            'hdr=""; url=""\n'
            "while [ $# -gt 0 ]; do\n"
            '  case "$1" in\n'
            '    --header-file) hdr="$2"; shift ;;\n'
            '    -o|-T|--max|--ca) shift ;;\n'
            '    --insecure) ;;\n'
            '    *) url="$1" ;;\n'
            "  esac\n"
            "  shift\n"
            "done\n"
            f'echo "$url header=$([ -n "$hdr" ] && cat "$hdr")" >> \'{self.calls}\'\n'
            'echo "hub_fetch: $url: HTTP 404" >&2; exit 1\n')
        hub_fetch.chmod(0o755)
        (self.root / "data" / "harmony" / "cacert.pem").write_text("x\n")

    def use_wget(self):
        (self.root / "stub" / "busybox").write_text(
            "#!/bin/sh\n"
            'if [ "$1" = wget ]; then\n'
            '  shift; hdr=""; url=""\n'
            "  while [ $# -gt 0 ]; do\n"
            '    case "$1" in\n'
            '      --header) hdr="$2"; shift ;;\n'
            '      -O|-T) shift ;;\n'
            '      -q) ;;\n'
            '      *) url="$1" ;;\n'
            "    esac\n"
            "    shift\n"
            "  done\n"
            f'  echo "$url header=$hdr" >> \'{self.calls}\'\n'
            "  exit 1\n"
            "fi\n"
            'exec "$@"\n')

    def lines(self) -> list[str]:
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def check_custom(self):
        self.run_update("check", "https://updates.example.invalid/ota", "tok123")
        calls = self.lines()
        self.assertTrue(calls, "the source was asked")
        self.assertFalse([c for c in calls if "api.github.com" in c], "no GitHub fallback for a custom source: %s" % calls)
        self.assertFalse([c for c in calls if "tok123" in c and not c.startswith("https://updates.example.invalid/")],
                         "the token reaches no other host: %s" % calls)

    def check_default(self):
        self.run_update("check", "", "tok123")
        gh = [c for c in self.lines() if c.startswith("https://api.github.com/")]
        self.assertTrue(gh, "the default source keeps the GitHub fallback: %s" % self.lines())
        self.assertFalse([c for c in gh if "tok123" in c], "never the token to GitHub: %s" % gh)

    def test_other_github_repo_hub_fetch(self):
        # a source on another GitHub repository: never the public project's release list; only
        # its own repository's (if the version check asks one at all), without the token
        self.use_hub_fetch()
        self.run_update("check", "https://raw.githubusercontent.com/someone/fork/ota", "tok123")
        gh = [c for c in self.lines() if "api.github.com" in c]
        self.assertFalse([c for c in gh if "/repos/someone/fork/" not in c], "not the public project's releases: %s" % gh)
        self.assertFalse([c for c in gh if "tok123" in c], "never the token to GitHub: %s" % gh)

    def test_custom_source_hub_fetch(self):
        self.use_hub_fetch()
        self.check_custom()

    def test_default_source_hub_fetch(self):
        self.use_hub_fetch()
        self.check_default()

    def test_custom_source_wget(self):
        self.use_wget()
        self.check_custom()

    def test_default_source_wget(self):
        self.use_wget()
        self.check_default()


class LockStarting(Sandbox):
    def start_of(self, pid: int) -> str:
        return pathlib.Path(f"/proc/{pid}/stat").read_text().split(") ")[1].split()[19]

    def test_lock_without_pid_file_is_not_taken_over(self):
        # a run between its mkdir and its pid file; it writes the file 0.5 s later
        self.holder = subprocess.Popen(["sh", "-c", "sleep 30; :"])
        lock = self.root / "tmp" / "ota_update.lock"
        lock.mkdir()
        run = subprocess.Popen(["sh", str(self.script), "repair-tls"], env=self.env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
        time.sleep(0.5)
        (lock / "pid").write_text(f"{self.holder.pid}\n")
        (lock / "start").write_text(self.start_of(self.holder.pid) + "\n")
        out, _ = run.communicate(timeout=60)
        self.assertIn("A software update is running", out)
        self.assertEqual(run.returncode, 1, out)
        self.assertEqual((lock / "pid").read_text().strip(), str(self.holder.pid), "the starting run keeps its lock")


class RepairLoopLog(Sandbox):
    def test_repair_log_bounded(self):
        text = INIT.read_text()
        m = re.search(r"\n(# hub_fetch or libbearssl\.so\.0 missing.*?\n  \) &\n.*?\nfi\n)", text, re.S)
        self.assertIsNotNone(m, "repair block of init.sh not found")
        block = self.moved(m.group(1)).replace("\n  ) &\n", "\n  )\n")
        upd = self.root / "data" / "harmony" / "update.sh"
        upd.write_text("#!/bin/sh\necho \"repair try failed: no network\"\nexit 1\n")
        upd.chmod(0o755)
        sleeps = self.root / "sleeps"
        stop = self.root / "tmp" / "harmony_stop"
        (self.root / "stub" / "sleep").write_text(
            f"#!/bin/sh\necho x >> '{sleeps}'\n"
            f"[ $(grep -c x '{sleeps}') -ge 12 ] && touch '{stop}'\nexit 0\n")
        (self.root / "stub" / "sleep").chmod(0o755)
        res = subprocess.run(["sh", "-c", block], env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stderr)
        tries = len(sleeps.read_text().splitlines())   # one sleep before the loop, one after each try
        log = (self.root / "tmp" / "update_repair.log").read_text().splitlines()
        self.assertGreater(tries - 1, 6, "the loop ran more than 6 tries")
        self.assertEqual(len(log), 1, "after 6 tries the log holds only the latest one: %d lines" % len(log))


if __name__ == "__main__":
    unittest.main()
