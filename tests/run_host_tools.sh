#!/bin/bash
# Tests of the host tools: the Python and shell scripts that run on the user's computer (install,
# deploy, backup, recovery, migration, firmware and build tools). No device contact: ssh, USB and
# the devices are replaced by fakes. The firmware tests are in tests/run_mock_suite.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/tmp/test_host"
mkdir -p "$BUILD_DIR"

echo "==========================================================="
echo "  Harmony Host Tools Test Suite (Python $(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])'))"
echo "==========================================================="

# Every tool stops with a clear message on Python older than 3.13 (docs/FIRST_INSTALL_*.md)
python3 "$SCRIPT_DIR/test_python_minimum.py"

# build/deploy_hub.py: services started again after an early end (ssh and npm replaced), free space
python3 "$SCRIPT_DIR/test_deploy_hub_restart.py"
python3 "$SCRIPT_DIR/test_deploy_hub_ssh_guard.py"
python3 "$SCRIPT_DIR/test_deploy_hub_space.py"
# build/deploy_hub.py: the files of the removed recovery access point go from older Hubs
python3 "$SCRIPT_DIR/test_deploy_hub_recovery_ap.py"
# build/gzip_assets.py: the output does not change between builds of the same input
python3 "$SCRIPT_DIR/test_gzip_assets_reproducible.py"

# remote/setup_remote.py: --no-screen writes remote_settings.conf, release bundle layout, SSH key names
python3 "$SCRIPT_DIR/test_setup_remote_no_screen.py"
python3 "$SCRIPT_DIR/test_setup_remote_bundle.py"
python3 "$SCRIPT_DIR/test_host_tools_ssh.py"
# remote/deploy_remote.py: stops before the restart when an upload fails (fake remote)
python3 "$SCRIPT_DIR/test_deploy_remote_failures.py"
# USB root boot scripts of tools/root_hub_usb.py and remote/setup_remote.py (refuse the other device class)
python3 "$SCRIPT_DIR/test_root_boot_scripts.py"

# tools/backup_hub.py keeps only complete tar archives (fake ssh)
python3 "$SCRIPT_DIR/test_backup_hub_check.py"
# tools/factory_reset.py stops and exits non-zero when a step fails (fake ssh)
python3 "$SCRIPT_DIR/test_factory_reset_tool.py"
# tools/restore_stock.py against a fake Hub root (no network)
python3 "$SCRIPT_DIR/test_restore_stock.py"
# tools/harmony_hid.py reports a cut file write (fake USB HID transport)
python3 "$SCRIPT_DIR/test_hid_write_result.py"
# tools/harmony_firmware.py flash flow (fake USB device)
python3 "$SCRIPT_DIR/test_harmony_firmware_flash.py"
# tools/migrate_hub_configs.py: tar.gz members stay in the extraction folder, transport and isMacro kept
python3 "$SCRIPT_DIR/test_migrate_tar_paths.py"
python3 "$SCRIPT_DIR/test_migrate_keep_transport.py"

# Test groups of the host tools kept in their own files (tests/suite.d/*host-tools*.sh)
for suite_part in "$SCRIPT_DIR"/suite.d/*host-tools*.sh; do
    [ -e "$suite_part" ] || continue
    echo "[-] Test group $(basename "$suite_part" .sh)"
    . "$suite_part"
done

echo "==========================================================="
echo "  ALL HOST TOOLS TESTS COMPLETED WITH 100% PASS"
echo "==========================================================="
