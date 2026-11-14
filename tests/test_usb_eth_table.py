#!/usr/bin/env python3
"""Every USB Ethernet adapter ID in the network guide must reach a driver that binds it.

For each row of the "Supported USB Ethernet Chipsets" table in
docs/ETHERNET_AND_NETWORK_SETUP.md that lists IDs, the test checks that
1. the chipset module named in the row binds the ID (its alias=usb:v<vid>p<pid> strings
   in payload/modules/<module>.ko), and
2. payload/scripts/start_usb_eth.sh, run in a host sandbox with that ID on USB port 1-1
   (insmod and the other device commands are stubs), loads that module.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
DOC = REPO_DIR / "docs" / "ETHERNET_AND_NETWORK_SETUP.md"
SCRIPT = REPO_DIR / "payload" / "scripts" / "start_usb_eth.sh"
MODULES = REPO_DIR / "payload" / "modules"
STUBS = ("killall", "umount", "rmmod", "sleep")


def table_rows() -> list[tuple[str, list[str], str]]:
    text = DOC.read_text()
    section = text[text.index("### Supported USB Ethernet Chipsets"):]
    section = section[:section.index("\n---")]
    rows = []
    for line in section.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3:
            continue
        ids = re.findall(r"`([0-9A-Fa-f]{4}:[0-9A-Fa-f]{4})`", cells[1])
        modules = [m for m in re.findall(r"`(\w+)\.ko`", cells[2]) if m != "usbnet"]
        if ids and modules:
            rows.append((cells[0], ids, modules[0]))
    return rows


def module_aliases(module: str) -> set[str]:
    data = (MODULES / f"{module}.ko").read_bytes()
    return {f"{v}:{p}".upper() for v, p in re.findall(rb"alias=usb:v([0-9A-F]{4})p([0-9A-F]{4})", data)
            for v, p in [(v.decode(), p.decode())]}


class UsbEthTable(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def loaded_modules(self, usb_id: str) -> list[str]:
        root = self.base / usb_id.replace(":", "_")
        dev = root / "sys" / "bus" / "usb" / "devices" / "1-1"
        dev.mkdir(parents=True)
        vid, pid = usb_id.lower().split(":")
        (dev / "idVendor").write_text(vid + "\n")
        (dev / "idProduct").write_text(pid + "\n")
        mods = root / "data" / "harmony" / "modules"
        mods.mkdir(parents=True)
        for ko in MODULES.glob("*.ko"):
            (mods / ko.name).write_text("")
        (root / "sys" / "devices" / "platform" / "ar7240-ehci.0" / "driver").mkdir(parents=True)
        stub = root / "stub"
        stub.mkdir()
        log = root / "insmod.log"
        for name in STUBS:
            (stub / name).write_text("#!/bin/sh\nexit 0\n")
        (stub / "insmod").write_text(f"#!/bin/sh\necho \"$1\" >> '{log}'\n")
        for p in stub.iterdir():
            p.chmod(0o755)
        text = re.sub(r"(?<![\w./$-])/(data|sys|cache|lib|usr)/", lambda m: f"{root}/{m.group(1)}/", SCRIPT.read_text())
        script = root / "start_usb_eth.sh"
        script.write_text(text)
        env = dict(os.environ, PATH=f"{stub}:/usr/bin:/bin")
        subprocess.run(["sh", str(script)], env=env, capture_output=True, text=True, timeout=30)
        return [pathlib.Path(x).stem for x in log.read_text().split()] if log.exists() else []

    def test_table_ids_reach_their_driver(self):
        rows = table_rows()
        self.assertGreaterEqual(len(rows), 4, rows)
        problems = []
        for family, ids, module in rows:
            bound = module_aliases(module)
            for usb_id in ids:
                if usb_id.upper() not in bound:
                    problems.append(f"{family}: {usb_id} is not bound by {module}.ko")
                loaded = self.loaded_modules(usb_id)
                if module not in loaded:
                    problems.append(f"{family}: start_usb_eth.sh loads {loaded} for {usb_id}, not {module}.ko")
        self.assertEqual(problems, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
