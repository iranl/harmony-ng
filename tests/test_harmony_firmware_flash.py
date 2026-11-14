#!/usr/bin/env python3
"""tools/harmony_firmware.py flash flow with a fake USB device (no USB, no network)."""

from __future__ import annotations

import pathlib
import sys
import unittest
from unittest.mock import patch

REPO_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from tools import harmony_firmware as fw  # noqa: E402


class FakeHid:
    present: set[int] = set()
    write_ok = True
    checksum_ok = True
    calls: list[str] = []

    def __init__(self, pid: int):
        self.pid = pid

    def open(self) -> bool:
        return self.pid in FakeHid.present

    def close(self) -> None:
        pass

    def reset_filesystem(self) -> bool:
        return True

    def open_file_write(self, path: str, size: int) -> int:
        FakeHid.calls.append(f"open_file_write {path}")
        return 1

    def write_file_chunks(self, handle_id, data, packets_per_chunk=5, progress_callback=None) -> bool:
        FakeHid.calls.append("write_file_chunks")
        return FakeHid.write_ok

    def verify_checksum(self, **kwargs):
        FakeHid.calls.append("verify_checksum")
        return (True, "match") if FakeHid.checksum_ok else (False, "status_mismatch")

    def close_file(self, handle_id) -> bool:
        FakeHid.calls.append("close_file")
        return True

    def finalize_reset(self) -> bool:
        FakeHid.calls.append("finalize_reset")
        return True


def hub_bundle() -> fw.FirmwareBundle:
    img = fw.FirmwareImage(
        name="ota-update.EzHex",
        remote_path="/fw/otaupdate",
        operation_type="write",
        size=4,
        checksum_type="MD5",
        checksum_expected="00" * 16,
        checksum_length=4,
        checksum_offset=0,
        checksum_seed=0,
        data=b"\0\0\0\0",
    )
    return fw.FirmwareBundle(pathlib.Path("hub_97_test.hfw2"), [97], [img], [{"reset": True}])


class FlashFirmware(unittest.TestCase):
    def setUp(self):
        FakeHid.present = {fw.HUB_PID}
        FakeHid.write_ok = True
        FakeHid.checksum_ok = True
        FakeHid.calls = []
        p = patch.object(fw, "HarmonyHid", FakeHid)
        p.start()
        self.addCleanup(p.stop)

    def test_failed_write_does_not_reset(self):
        FakeHid.write_ok = False
        self.assertFalse(fw.flash_firmware(hub_bundle(), target_device="hub", skip_confirm=True))
        self.assertNotIn("finalize_reset", FakeHid.calls)
        self.assertNotIn("verify_checksum", FakeHid.calls)
        self.assertIn("close_file", FakeHid.calls)

    def test_failed_checksum_does_not_reset(self):
        FakeHid.checksum_ok = False
        self.assertFalse(fw.flash_firmware(hub_bundle(), target_device="hub", skip_confirm=True))
        self.assertIn("verify_checksum", FakeHid.calls)
        self.assertNotIn("finalize_reset", FakeHid.calls)
        self.assertIn("close_file", FakeHid.calls)

    def test_good_write_resets(self):
        self.assertTrue(fw.flash_firmware(hub_bundle(), target_device="hub", skip_confirm=True))
        self.assertEqual(FakeHid.calls[-1], "finalize_reset")

    def test_target_selects_the_bundle(self):
        """Remote and Hub both on USB, --target hub: the Hub bundle is fetched, not the remote's."""
        FakeHid.present = {fw.REMOTE_PID, fw.HUB_PID}
        seen: dict[str, object] = {}

        def fake_parse(path):
            seen["bundle"] = pathlib.Path(path).name
            return hub_bundle()

        def fake_flash(bundle, target_device, dry_run, skip_confirm):
            seen["target"] = target_device
            return True

        with patch.object(fw, "query_latest_firmware", return_value={"URI": "https://example.invalid/hub", "Id": "4.15.600"}), \
             patch.object(fw, "query_latest_remote_firmware", return_value=({"URI": "https://example.invalid/remote", "Id": "4.15.330"}, 111)), \
             patch.object(fw, "download_file", return_value=True), \
             patch.object(fw, "parse_hfw2_bundle", side_effect=fake_parse), \
             patch.object(fw, "flash_firmware", side_effect=fake_flash), \
             patch.object(sys, "argv", ["harmony_firmware.py", "flash", "--target", "hub", "--yes"]):
            fw.main()
        self.assertEqual(seen["target"], "hub")
        self.assertTrue(str(seen["bundle"]).startswith("hub_97_"), seen["bundle"])

    def test_auto_target_passes_the_detected_device(self):
        """--target auto: the device found by the probe is flashed, flash_firmware does not probe again."""
        FakeHid.present = {fw.HUB_PID}
        seen: dict[str, object] = {}

        def fake_flash(bundle, target_device, dry_run, skip_confirm):
            seen["target"] = target_device
            return True

        with patch.object(fw, "query_latest_firmware", return_value={"URI": "https://example.invalid/hub", "Id": "4.15.600"}), \
             patch.object(fw, "download_file", return_value=True), \
             patch.object(fw, "parse_hfw2_bundle", return_value=hub_bundle()), \
             patch.object(fw, "flash_firmware", side_effect=fake_flash), \
             patch.object(sys, "argv", ["harmony_firmware.py", "flash", "--yes"]):
            fw.main()
        self.assertEqual(seen["target"], "hub")


if __name__ == "__main__":
    unittest.main(verbosity=2)
