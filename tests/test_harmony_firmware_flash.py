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
    sysinfo: dict[str, str] = {"skin": "97"}
    calls: list[str] = []

    def __init__(self, pid: int):
        self.pid = pid

    def get_sysinfo(self) -> dict[str, str]:
        return dict(FakeHid.sysinfo)

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


def hub_bundle(skins: list[int] | None = None) -> fw.FirmwareBundle:
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
    return fw.FirmwareBundle(pathlib.Path("hub_97_test.hfw2"), [97] if skins is None else skins, [img], [{"reset": True}])


# Intended skins of the published remote bundle (Description.xml of remote_111_4.15.330.hfw2)
REMOTE_BUNDLE_SKINS = [99, 100, 102, 105, 108, 111, 112]


class FlashFirmware(unittest.TestCase):
    def setUp(self):
        FakeHid.present = {fw.HUB_PID}
        FakeHid.write_ok = True
        FakeHid.checksum_ok = True
        FakeHid.sysinfo = {"skin": "97"}
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

    def test_bundle_for_another_skin_is_refused(self):
        """A remote bundle on a Hub (skin 97): nothing written, no reset."""
        self.assertFalse(fw.flash_firmware(hub_bundle(REMOTE_BUNDLE_SKINS), target_device="hub", skip_confirm=True))
        self.assertEqual(FakeHid.calls, [])

    def test_pro_remote_skin_not_in_the_remote_bundle(self):
        """Skin 116 (Pro 2400 remote) is not in the published remote bundle: refused."""
        FakeHid.present = {fw.REMOTE_PID}
        FakeHid.sysinfo = {"skin": "116"}
        self.assertFalse(fw.flash_firmware(hub_bundle(REMOTE_BUNDLE_SKINS), target_device="remote", skip_confirm=True))
        self.assertEqual(FakeHid.calls, [])

    def test_remote_fallback_bundle_is_allowed(self):
        """Skin 116 has no bundle of its own: the skin 111 bundle chosen as its fallback is flashed."""
        FakeHid.present = {fw.REMOTE_PID}
        FakeHid.sysinfo = {"skin": "116"}
        self.assertTrue(fw.flash_firmware(hub_bundle(REMOTE_BUNDLE_SKINS), target_device="remote",
                                          skip_confirm=True, fallback_skin=111))
        self.assertEqual(FakeHid.calls[-1], "finalize_reset")

    def test_fallback_does_not_allow_another_device_class(self):
        """The fallback is for remotes only: a Hub bundle on a remote, or a remote bundle on a Hub, stays refused."""
        FakeHid.present = {fw.REMOTE_PID}
        FakeHid.sysinfo = {"skin": "116"}
        self.assertFalse(fw.flash_firmware(hub_bundle(), target_device="remote", skip_confirm=True, fallback_skin=111))
        FakeHid.present = {fw.HUB_PID}
        FakeHid.sysinfo = {"skin": "97"}
        self.assertFalse(fw.flash_firmware(hub_bundle(REMOTE_BUNDLE_SKINS), target_device="hub",
                                           skip_confirm=True, fallback_skin=111))
        self.assertEqual(FakeHid.calls, [])

    def test_auto_remote_passes_its_fallback_skin(self):
        """flash without --firmware on a remote: the latest remote bundle is the deliberate fallback."""
        FakeHid.present = {fw.REMOTE_PID}
        seen: dict[str, object] = {}

        def fake_flash(bundle, target_device, dry_run, skip_confirm, force=False, fallback_skin=None):
            seen["fallback_skin"] = fallback_skin
            return True

        with patch.object(fw, "query_latest_remote_firmware", return_value=({"URI": "https://example.invalid/remote", "Id": "4.15.330"}, 111)), \
             patch.object(fw, "download_file", return_value=True), \
             patch.object(fw, "parse_hfw2_bundle", return_value=hub_bundle(REMOTE_BUNDLE_SKINS)), \
             patch.object(fw, "flash_firmware", side_effect=fake_flash), \
             patch.object(sys, "argv", ["harmony_firmware.py", "flash", "--yes"]):
            fw.main()
        self.assertEqual(seen["fallback_skin"], 111)

    def test_hex_skin_in_the_bundle_is_flashed(self):
        """A remote reporting its skin as hex text (0x64 = 100, Ultimate 2013) gets the remote bundle."""
        FakeHid.present = {fw.REMOTE_PID}
        FakeHid.sysinfo = {"skin": "0x64"}
        self.assertTrue(fw.flash_firmware(hub_bundle(REMOTE_BUNDLE_SKINS), target_device="remote", skip_confirm=True))
        self.assertEqual(FakeHid.calls[-1], "finalize_reset")

    def test_unknown_skin_or_no_skin_list_is_refused(self):
        FakeHid.sysinfo = {}
        self.assertFalse(fw.flash_firmware(hub_bundle(), target_device="hub", skip_confirm=True))
        FakeHid.sysinfo = {"skin": "97"}
        self.assertFalse(fw.flash_firmware(hub_bundle([]), target_device="hub", skip_confirm=True))
        self.assertEqual(FakeHid.calls, [])

    def test_dry_run_reports_the_mismatch(self):
        self.assertFalse(fw.flash_firmware(hub_bundle(REMOTE_BUNDLE_SKINS), target_device="hub", dry_run=True))
        self.assertTrue(fw.flash_firmware(hub_bundle(), target_device="hub", dry_run=True))
        self.assertEqual(FakeHid.calls, [])

    def test_force_flashes_anyway(self):
        self.assertTrue(fw.flash_firmware(hub_bundle(REMOTE_BUNDLE_SKINS), target_device="hub", skip_confirm=True, force=True))
        self.assertEqual(FakeHid.calls[-1], "finalize_reset")

    def test_force_option_reaches_the_flasher(self):
        seen: dict[str, object] = {}

        def fake_flash(bundle, target_device, dry_run, skip_confirm, force=False, fallback_skin=None):
            seen["force"] = force
            return True

        with patch.object(fw, "parse_hfw2_bundle", return_value=hub_bundle()), \
             patch.object(fw, "flash_firmware", side_effect=fake_flash), \
             patch.object(sys, "argv", ["harmony_firmware.py", "flash", "--firmware", "x.hfw2", "--yes", "--force"]):
            fw.main()
        self.assertIs(seen["force"], True)

    def test_target_selects_the_bundle(self):
        """Remote and Hub both on USB, --target hub: the Hub bundle is fetched, not the remote's."""
        FakeHid.present = {fw.REMOTE_PID, fw.HUB_PID}
        seen: dict[str, object] = {}

        def fake_parse(path):
            seen["bundle"] = pathlib.Path(path).name
            return hub_bundle()

        def fake_flash(bundle, target_device, dry_run, skip_confirm, force=False, fallback_skin=None):
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

        def fake_flash(bundle, target_device, dry_run, skip_confirm, force=False, fallback_skin=None):
            seen["target"] = target_device
            return True

        with patch.object(fw, "query_latest_firmware", return_value={"URI": "https://example.invalid/hub", "Id": "4.15.600"}), \
             patch.object(fw, "download_file", return_value=True), \
             patch.object(fw, "parse_hfw2_bundle", return_value=hub_bundle()), \
             patch.object(fw, "flash_firmware", side_effect=fake_flash), \
             patch.object(sys, "argv", ["harmony_firmware.py", "flash", "--yes"]):
            fw.main()
        self.assertEqual(seen["target"], "hub")


class SetupRemoteFallback(unittest.TestCase):
    """remote/setup_remote.py step 1 passes the skin 111 fallback to the flasher only when it chose it."""

    def run_step1(self, device_skin: int, own_bundle: bool) -> dict[str, object]:
        import importlib.util
        import contextlib
        import io
        from tools import harmony_hid, usb_state  # noqa: F401  (patched below)

        spec = importlib.util.spec_from_file_location("setup_remote_under_test", REPO_DIR / "remote" / "setup_remote.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        seen: dict[str, object] = {}

        def fake_query(skin, silent_on_empty=False):
            seen.setdefault("queried", []).append(skin)
            if skin == device_skin and not own_bundle:
                return None
            return {"Id": "4.15.330", "URI": "https://example.invalid/remote"}

        def fake_flash(bundle, target_device="auto", dry_run=False, skip_confirm=False, force=False, fallback_skin=None):
            seen["fallback_skin"] = fallback_skin
            return False  # stop here: step 1 then goes on to the payload, stopped below

        FakeHid.present = {fw.REMOTE_PID}
        with patch.object(harmony_hid, "HarmonyHid", FakeHid), \
             patch.object(mod, "require_firmware_or_exit", return_value=("4.15.300", device_skin)), \
             patch.object(fw, "query_latest_firmware", side_effect=fake_query), \
             patch.object(fw, "download_file", return_value=True), \
             patch.object(fw, "parse_hfw2_bundle", return_value=hub_bundle(REMOTE_BUNDLE_SKINS)), \
             patch.object(fw, "flash_firmware", side_effect=fake_flash), \
             patch.object(pathlib.Path, "is_file", return_value=True), \
             patch.object(mod, "build_payload", side_effect=RuntimeError("stop after the firmware step")), \
             patch("builtins.input", return_value="y"), \
             patch.object(mod.time, "sleep", return_value=None), \
             contextlib.redirect_stdout(io.StringIO()):
            mod.step1_enable_usb_debug("ssh-ed25519 AAAA test")
        return seen

    def test_skin_without_own_bundle_uses_the_fallback(self):
        seen = self.run_step1(116, own_bundle=False)
        self.assertEqual(seen["queried"], [116, 111])
        self.assertEqual(seen["fallback_skin"], 111)

    def test_skin_with_own_bundle_has_no_fallback(self):
        seen = self.run_step1(112, own_bundle=True)
        self.assertIsNone(seen["fallback_skin"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
