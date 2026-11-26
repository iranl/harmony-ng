#!/usr/bin/env python3
"""USB Ethernet driver choice (payload/scripts/usb_eth_driver.sh with usb_eth_modules.txt).

1. Every adapter ID in the "Supported USB Ethernet Chipsets" table of
   docs/ETHERNET_AND_NETWORK_SETUP.md is bound by the module the row names (its
   alias=usb:v<vid>p<pid> strings in payload/modules/<module>.ko), and usb_eth_driver.sh scan,
   run in a host sandbox with that ID on USB, loads that module (insmod is a stub).
2. Modules are installed on demand: with none installed, scan loads nothing and writes the
   driver and the missing modules to /tmp/eth_driver_state.
3. An adapter behind a USB hub (1-1.2) is found; the hub itself and a USB stick are skipped.
4. An adapter no module of this firmware supports (AX88179) is reported as unsupported.
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
SCRIPT = REPO_DIR / "payload" / "scripts" / "usb_eth_driver.sh"
TABLE = REPO_DIR / "payload" / "scripts" / "usb_eth_modules.txt"
MODULES = REPO_DIR / "payload" / "modules"
STUBS = ("sleep",)


def table_rows() -> list[tuple[str, list[str], str]]:
    text = DOC.read_text(encoding="utf-8")
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


class UsbEthDriver(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.tmp.name)
        self.n = 0

    def tearDown(self):
        self.tmp.cleanup()

    def sandbox(self, devices: dict[str, dict[str, str]], installed: bool = True) -> pathlib.Path:
        """devices: name (1-1, 1-1.2) -> sysfs attributes; "if_class" makes interface 1.0."""
        self.n += 1
        root = self.base / str(self.n)
        usb = root / "sys" / "bus" / "usb" / "devices"
        usb.mkdir(parents=True)
        (usb / "usb1").mkdir()
        for name, attrs in devices.items():
            dev = usb / name
            dev.mkdir()
            for k, v in attrs.items():
                if k == "if_class":
                    # one interface per class, comma-separated (a composite device)
                    for i, cls in enumerate(v.split(",")):
                        (dev / f"{name}:1.{i}").mkdir()
                        (dev / f"{name}:1.{i}" / "bInterfaceClass").write_text(cls + "\n")
                else:
                    (dev / k).write_text(v + "\n")
        (root / "tmp").mkdir()
        # a /proc of its own: modules of the host (usbnet for a dock, say) must not count as loaded
        (root / "proc").mkdir()
        (root / "proc" / "modules").write_text("")
        harmony = root / "data" / "harmony"
        (harmony / "modules").mkdir(parents=True)
        (harmony / "usb_eth_modules.txt").write_text(TABLE.read_text(encoding="utf-8"))
        if installed:
            for ko in MODULES.glob("*.ko"):
                (harmony / "modules" / ko.name).write_text("")
        stub = root / "stub"
        stub.mkdir()
        for name in STUBS:
            (stub / name).write_text("#!/bin/sh\nexit 0\n")
        (stub / "insmod").write_text(f"#!/bin/sh\necho \"$1\" >> '{root}/insmod.log'\n")
        for p in stub.iterdir():
            p.chmod(0o755)
        text = re.sub(r"(?<![\w./$-])/(data|sys|cache|tmp|proc)/", lambda m: f"{root}/{m.group(1)}/",
                      SCRIPT.read_text(encoding="utf-8"))
        (root / "usb_eth_driver.sh").write_text(text)
        env = dict(os.environ, PATH=f"{stub}:/usr/bin:/bin")
        subprocess.run(["sh", str(root / "usb_eth_driver.sh"), "scan"], env=env, capture_output=True,
                       text=True, timeout=30)
        return root

    @staticmethod
    def loaded(root: pathlib.Path) -> list[str]:
        log = root / "insmod.log"
        return [pathlib.Path(x).stem for x in log.read_text().split()] if log.exists() else []

    @staticmethod
    def state(root: pathlib.Path) -> dict[str, str]:
        path = root / "tmp" / "eth_driver_state"
        if not path.exists():
            return {}
        return dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)

    @staticmethod
    def adapter(usb_id: str) -> dict[str, str]:
        vid, pid = usb_id.lower().split(":")
        return {"idVendor": vid, "idProduct": pid, "bDeviceClass": "00", "if_class": "ff"}

    def test_table_ids_reach_their_driver(self):
        rows = table_rows()
        self.assertGreaterEqual(len(rows), 4, rows)
        problems = []
        for family, ids, module in rows:
            bound = module_aliases(module)
            for usb_id in ids:
                if usb_id.upper() not in bound:
                    problems.append(f"{family}: {usb_id} is not bound by {module}.ko")
                root = self.sandbox({"1-1": self.adapter(usb_id)})
                if module not in self.loaded(root):
                    problems.append(f"{family}: usb_eth_driver.sh loads {self.loaded(root)} for {usb_id}, not {module}.ko")
                if self.state(root).get("loaded") != "1" and module not in self.loaded(root):
                    problems.append(f"{family}: state not loaded for {usb_id}")
        self.assertEqual(problems, [])

    def test_missing_driver_reported_not_loaded(self):
        root = self.sandbox({"1-1": self.adapter("0b95:7720")}, installed=False)
        self.assertEqual(self.loaded(root), [])
        st = self.state(root)
        self.assertEqual((st.get("vid"), st.get("pid"), st.get("driver")), ("0b95", "7720", "asix"))
        self.assertEqual(st.get("missing"), "usbnet asix")
        self.assertEqual(st.get("loaded"), "0")

    def test_adapter_behind_hub_and_stick_skipped(self):
        root = self.sandbox({
            "1-1": {"idVendor": "05e3", "idProduct": "0608", "bDeviceClass": "09"},
            "1-1.1": {"idVendor": "0781", "idProduct": "5567", "bDeviceClass": "00", "if_class": "08"},
            "1-1.2": self.adapter("0bda:8153"),
        })
        self.assertEqual(self.loaded(root), ["r8152"])
        self.assertEqual(self.state(root).get("driver"), "r8152")

    def test_unsupported_adapter(self):
        root = self.sandbox({"1-1": self.adapter("0b95:1790")})
        self.assertEqual(self.loaded(root), [])
        self.assertEqual(self.state(root).get("unsupported"), "1")

    def test_full_speed_adapter_loads_no_driver(self):
        # an OTG cable with a USB 1.1 hub: through the transaction translator the Hub freezes on
        # the adapter's first receive, so nothing is loaded; the stick (bulk only) still counts
        # as fine, and the same adapter at high speed gets its driver
        def bus(speed):
            return {
                "1-1": {"idVendor": "0a05", "idProduct": "7211", "bDeviceClass": "09", "speed": speed},
                "1-1.1": dict(self.adapter("0b95:772b"), speed=speed),
                "1-1.4": {"idVendor": "0781", "idProduct": "5581", "bDeviceClass": "00",
                          "if_class": "08", "speed": speed},
            }
        root = self.sandbox(bus("12"))
        self.assertEqual(self.loaded(root), [])
        st = self.state(root)
        self.assertEqual((st.get("vid"), st.get("pid"), st.get("slow"), st.get("loaded")),
                         ("0b95", "772b", "1", "0"))
        root = self.sandbox(bus("480"))
        self.assertEqual(self.loaded(root), ["usbnet", "asix"])
        self.assertEqual(self.state(root).get("slow"), "0")
        # a full-speed stick alone is no reason to report anything
        root = self.sandbox({"1-1.4": bus("12")["1-1.4"]})
        self.assertEqual(self.state(root), {})
        # a full-speed composite device with storage and a network interface does count
        root = self.sandbox({
            "1-1": dict(self.adapter("0bda:8153"), speed="480"),
            "1-1.2": {"idVendor": "1234", "idProduct": "5678", "bDeviceClass": "00",
                      "if_class": "08,ff", "speed": "12"},
        })
        self.assertEqual(self.loaded(root), [])
        self.assertEqual(self.state(root).get("slow"), "1")

    def test_installed_module_downloaded_again(self):
        # a module file that is there but does not load (an old build) is fetched again
        root = self.sandbox({"1-1": self.adapter("0b95:7720")})
        fetched = root / "fetched.log"
        update = root / "data" / "harmony" / "update.sh"
        update.write_text(f"#!/bin/sh\necho \"$*\" >> '{fetched}'\n")
        update.chmod(0o755)
        subprocess.run(["sh", str(root / "usb_eth_driver.sh"), "install", "asix"],
                       env=dict(os.environ, PATH=f"{root / 'stub'}:/usr/bin:/bin"),
                       capture_output=True, text=True, timeout=30)
        self.assertEqual(fetched.read_text().split("\n")[:2], ["fetch-module usbnet", "fetch-module asix"])

    def test_no_adapter_no_state(self):
        root = self.sandbox({"1-1": {"idVendor": "0781", "idProduct": "5567", "bDeviceClass": "00",
                                     "if_class": "08"}})
        self.assertEqual(self.loaded(root), [])
        self.assertEqual(self.state(root), {})


if __name__ == "__main__":
    unittest.main(verbosity=2)
