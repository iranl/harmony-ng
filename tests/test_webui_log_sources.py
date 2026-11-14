#!/usr/bin/env python3
"""The web UIs' log viewers must offer only log sources the Hub serves.

GET /api/logs (payload/source/http_api_system.c) knows a fixed set of source names and
answers anything else with the hub_daemon log, labelled as what was asked for. The test
reads that set from the C source and checks the option values of the log source lists
in the new web UI (frontend/src/views/setup/system.js) and the legacy UI
(android-app/index.html).
"""

from __future__ import annotations

import pathlib
import re
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent


def hub_sources() -> set[str]:
    text = (REPO_DIR / "payload" / "source" / "http_api_system.c").read_text()
    route = text[text.index('"/api/logs"'):]
    route = route[:route.index("send_cjson_resp")]
    return {"daemon"} | set(re.findall(r'"source=(\w+)"', route))


class WebUiLogSources(unittest.TestCase):
    def test_new_ui(self):
        text = (REPO_DIR / "frontend" / "src" / "views" / "setup" / "system.js").read_text()
        block = text[text.index("log-source-select"):]
        block = block[:block.index("]),")]
        values = re.findall(r"h\('option', \{ value: '([^']+)'", block)
        self.assertTrue(values, "log source options not found")
        self.assertEqual(sorted(set(values) - hub_sources()), [], "options the Hub answers with the hub_daemon log")

    def test_legacy_ui(self):
        text = (REPO_DIR / "android-app" / "index.html").read_text()
        block = text[text.index('id="sysLogSource"'):]
        block = block[:block.index("</select>")]
        values = re.findall(r'<option value="([^"]+)"', block)
        self.assertTrue(values)
        self.assertEqual(sorted(set(values) - hub_sources()), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
