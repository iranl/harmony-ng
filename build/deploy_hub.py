#!/usr/bin/env python3
"""Deploy Harmony Hub Firmware, WebUI, and Configuration over SSH.

Unified cross-platform Python implementation based on build/deploy_hub.sh.

Usage:
    python build/deploy_hub.py <HUB_IP> [options]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time


def find_default_ssh_key() -> str | None:
    """Find newest harmony_owner key or standard SSH key."""
    env_key = os.environ.get("SSH_KEY")
    if env_key and os.path.isfile(env_key):
        return env_key

    home = pathlib.Path.home()
    ssh_dir = home / ".ssh"
    if ssh_dir.is_dir():
        owner_keys = sorted(
            [p for p in ssh_dir.glob("harmony_owner_*") if not p.name.endswith(".pub")],
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if owner_keys:
            return str(owner_keys[0])

        for std in ("id_rsa", "id_ed25519"):
            cand = ssh_dir / std
            if cand.is_file():
                return str(cand)

    return None


def get_file_md5(path: pathlib.Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest().lower()


class HubDeployer:
    def __init__(
        self,
        hub_host: str,
        hub_user: str = "root",
        ssh_key: str | None = None,
        ui_choice: str = "legacy",
        webui_only: bool = False,
        reboot_hub: bool = False,
        dry_run: bool = False,
        no_backup: bool = False,
        stage_remote_ota: bool = False,
    ):
        self.hub_host = hub_host
        self.hub_user = hub_user
        self.ssh_key = ssh_key or find_default_ssh_key()
        self.ui_choice = "legacy" if ui_choice in ("legacy", "old", "android-app") else "new"
        self.webui_only = webui_only
        self.reboot_hub = reboot_hub
        self.dry_run = dry_run
        self.no_backup = no_backup
        self.stage_remote_ota = stage_remote_ota

        self.deploy_failures: list[str] = []
        self.remote_hashes: dict[str, str] = {}

        # Resolve paths
        self.script_dir = pathlib.Path(__file__).resolve().parent
        self.repo_root = self.script_dir.parent

        if (self.script_dir / "bin").is_dir():
            # Release bundle layout
            self.bin_dir = self.script_dir / "bin"
            self.www_dir = self.script_dir / "www"
            self.scripts_dir = self.script_dir / "scripts"
            self.modules_dir = self.script_dir / "modules"
            self.config_dir = self.script_dir / "config"
            self.fw_tool = self.script_dir / "tools" / "harmony_firmware.py"
            self.icons_tar = self.script_dir / "icons.tar.gz"
        else:
            # Repository layout
            self.bin_dir = self.repo_root / "payload" / "bin"
            self.www_dir = self.repo_root / "payload" / "www"
            self.scripts_dir = self.repo_root / "payload" / "scripts"
            self.modules_dir = self.repo_root / "payload" / "modules"
            self.config_dir = self.repo_root / "payload" / "config"
            self.fw_tool = self.repo_root / "tools" / "harmony_firmware.py"
            self.icons_tar = self.repo_root / "build" / "icons.tar.gz"

        if (self.repo_root / "build" / "icons.tar.gz").is_file():
            self.icons_tar = self.repo_root / "build" / "icons.tar.gz"
        elif (self.script_dir / "icons.tar.gz").is_file():
            self.icons_tar = self.script_dir / "icons.tar.gz"

    def get_ssh_cmd(self, cmd_str: str) -> list[str]:
        cmd = [
            "ssh",
            "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=no",
            "-o", "ConnectTimeout=5",
        ]
        if self.ssh_key and os.path.isfile(self.ssh_key):
            cmd.extend(["-i", self.ssh_key])
        cmd.extend([f"{self.hub_user}@{self.hub_host}", cmd_str])
        return cmd

    def run_ssh(self, cmd_str: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(self.get_ssh_cmd(cmd_str), capture_output=True, text=True)

    def dry_skip(self, msg: str) -> bool:
        if self.dry_run:
            print(f"    [dry-run] would {msg}")
            return True
        return False

    def deploy_file_if_changed(self, src: pathlib.Path, dst: str, perm: str = "755") -> bool:
        if not src.is_file():
            return False

        fname = src.name
        local_md5 = get_file_md5(src)
        remote_md5 = self.remote_hashes.get(dst)

        if remote_md5 and remote_md5 == local_md5:
            print(f"    -> {fname} (MD5 match: {local_md5} - skipped)")
            return True

        if self.dry_run:
            print(f"    -> Would upload {fname} to {dst} (local: {local_md5} != remote: {remote_md5 or 'missing'})")
            return True

        print(f"    -> Uploading {fname} (local: {local_md5} != remote: {remote_md5 or 'missing'})...")
        ddir = str(pathlib.PurePosixPath(dst).parent)

        # Check flash free space on Hub
        size_kb = (src.stat().st_size + 1023) // 1024
        space_check = (
            f"mkdir -p '{ddir}' 2>/dev/null; rm -f '{dst}.new' 2>/dev/null; "
            f"a=$(df -k '{ddir}' 2>/dev/null | awk 'END {{print $(NF-2)}}'); o=0; "
            f"[ -f '{dst}' ] && o=$(ls -l '{dst}' 2>/dev/null | awk '{{print int(($5+1023)/1024)}}'); "
            f'echo "${{a:-unknown}} ${{o:-0}}"'
        )
        res = self.run_ssh(space_check)
        parts = res.stdout.strip().split()
        avail_kb_str = parts[0] if parts else ""
        old_kb = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
        avail_kb = int(avail_kb_str) if avail_kb_str.isdigit() else None

        need_kb = size_kb + 32  # flash headroom
        if avail_kb is None or avail_kb >= need_kb:
            stage = f"{dst}.new"
        elif (avail_kb + old_kb) >= need_kb:
            stage = f"/tmp/deploy.{fname}"
            print(f"       Not enough space for both copies ({avail_kb} KB free, {size_kb} KB needed); replacing in place.")
        else:
            print(
                f"    [-] {fname}: {ddir} has {avail_kb} KB free and old copy frees {old_kb} KB, but {need_kb} KB needed. "
                "Free space on Hub and deploy again.",
                file=sys.stderr,
            )
            self.deploy_failures.append(f"{dst} (no space)")
            return False

        # SCP upload
        scp_cmd = ["scp", "-O", "-q", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no"]
        if self.ssh_key and os.path.isfile(self.ssh_key):
            scp_cmd.extend(["-i", self.ssh_key])
        scp_cmd.extend([str(src), f"{self.hub_user}@{self.hub_host}:{stage}"])

        upload_res = subprocess.run(scp_cmd, capture_output=True, text=True)
        if upload_res.returncode != 0:
            self.run_ssh(f"rm -f '{stage}' 2>/dev/null")
            print(f"    [-] {fname}: upload failed; Hub keeps old copy.", file=sys.stderr)
            self.deploy_failures.append(f"{dst} (upload failed)")
            return False

        # Remote verify and atomic install
        install_script = (
            f"s='{stage}'; d='{dst}'; m=$(md5sum \"$s\" 2>/dev/null | cut -d' ' -f1); "
            f'if [ "$m" != "{local_md5}" ]; then rm -f "$s"; echo staged-mismatch; exit; fi; '
            f"chmod {perm} \"$s\" 2>/dev/null; "
            f'if [ "$s" = "$d.new" ]; then mv -f "$s" "$d" && echo ok || echo move-failed; exit; fi; '
            f'rm -f "$d"; cp "$s" "$d" 2>/dev/null; m=$(md5sum "$d" 2>/dev/null | cut -d\' \' -f1); '
            f'if [ "$m" = "{local_md5}" ]; then chmod {perm} "$d" 2>/dev/null; rm -f "$s"; echo ok; else rm -f "$d"; echo copy-failed; fi'
        )
        res = self.run_ssh(install_script)
        status = res.stdout.strip()
        if status == "ok":
            return True
        elif status == "staged-mismatch":
            print(f"    [-] {fname}: uploaded copy MD5 mismatch; Hub keeps old copy.", file=sys.stderr)
        elif status == "copy-failed":
            print(f"    [-] {fname}: writing {dst} failed and old copy gone. Temp copy in {stage}.", file=sys.stderr)
        else:
            print(f"    [-] {fname}: replacing {dst} failed ({status or 'no reply'}).", file=sys.stderr)

        self.deploy_failures.append(f"{dst} ({status or 'no reply'})")
        return False

    def query_remote_hashes(self, pattern: str) -> None:
        res = self.run_ssh(f"md5sum {pattern} 2>/dev/null || true")
        self.remote_hashes = {}
        for line in res.stdout.splitlines():
            parts = line.strip().split()
            if len(parts) >= 2:
                self.remote_hashes[parts[1]] = parts[0].lower()

    def deploy(self) -> bool:
        print("=== Harmony Hub Firmware Deployment ===")
        print(f"Target Hub: {self.hub_host}")
        print(f"SSH key: {self.ssh_key or '(ssh defaults)'}")
        print("Mode: Extended BusyBox & Harmony Runtime")

        # Connection check
        if self.dry_run:
            print("Dry run: Hub is only read, nothing is changed")
        probe = self.run_ssh("true")
        if probe.returncode != 0:
            print(f"[-] Cannot reach {self.hub_user}@{self.hub_host} over SSH.", file=sys.stderr)
            return False

        if not self.webui_only:
            # 1. Run automated backup first
            if not self.no_backup:
                print("[1/5] Running pre-deployment backup...")
                if not self.dry_skip(f"run backup_hub {self.hub_host}"):
                    try:
                        # Import and run backup_hub directly
                        # Release bundle: tools/ next to this script; repository: <repo>/tools
                        for tools_dir in (self.repo_root / "tools", self.script_dir / "tools"):
                            if (tools_dir / "backup_hub.py").is_file():
                                sys.path.insert(0, str(tools_dir))
                        import backup_hub as bkp_mod
                        bkp_mod.backup_hub(hub_host=self.hub_host, hub_user=self.hub_user, ssh_key=self.ssh_key)
                    except Exception as e:
                        print(f"[!] Warning running backup: {e}")

            # Keep stock startup script
            if not self.dry_skip("save stock /etc/init.d/rcS.local to /data/stock_backup (if not saved yet)"):
                self.run_ssh(
                    "mkdir -p /data/stock_backup; "
                    "if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then "
                    "  if ! grep -q 'elite' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'remote_' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'harmony' /etc/init.d/rcS.local 2>/dev/null; then "
                    "    cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; "
                    "    echo '[+] Saved /etc/init.d/rcS.local to /data/stock_backup/rcS.local.orig'; "
                    "  fi; "
                    "fi"
                )

            # 2. Stop existing processes cleanly
            print("[2/5] Stopping old hub processes cleanly...")
            stop_cmd = (
                "touch /tmp/harmony_stop 2>/dev/null || true; "
                "if [ -x /data/harmony/init.sh ]; then /data/harmony/init.sh stop; "
                "elif [ -x /data/harmony/scripts/init.sh ]; then /data/harmony/scripts/init.sh stop; "
                "else killall -15 hub_rf 2>/dev/null || true; sleep 1; "
                "killall -9 hub_rf hub_daemon hub_btstack hub_dhcpd hub_portal hal luaworks luadraws lua 2>/dev/null || true; fi; "
                "for _p in $(ps | grep '/data/codex/' | sed 's/^[ ]*//' | cut -d' ' -f1 2>/dev/null); do [ -n \"$_p\" ] && [ \"$_p\" != \"$$\" ] && kill -9 \"$_p\" 2>/dev/null || true; done; "
                "for _p in $(ps | grep 'init\\.sh' | sed 's/^[ ]*//' | cut -d' ' -f1 2>/dev/null); do [ -n \"$_p\" ] && [ \"$_p\" != \"$$\" ] && kill -9 \"$_p\" 2>/dev/null || true; done; "
                "/data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true"
            )
            if not self.dry_skip("stop hub services (init.sh stop)"):
                self.run_ssh(stop_cmd)

            # 3. Migrate legacy codex paths
            print("[3/5] Migrating legacy codex paths and querying remote checksums...")
            mig_paths_cmd = (
                "if [ -d /data/codex ] && [ ! -L /data/codex ]; then "
                "  if [ ! -d /data/harmony ]; then mv /data/codex /data/harmony 2>/dev/null || true; "
                "  else cp -rn /data/codex/* /data/harmony/ 2>/dev/null || true; rm -rf /data/codex 2>/dev/null || true; fi; fi; "
                "rm -f /data/codex /data/codex_p 2>/dev/null || true; "
                "[ -f /data/harmony/mqtt_config.json ] && [ ! -f /data/harmony/mqtt.json ] && mv -f /data/harmony/mqtt_config.json /data/harmony/mqtt.json 2>/dev/null || true; "
                "[ -f /data/harmony/elite_remote_map.json ] && [ ! -f /data/harmony/remote_slot1_map.json ] && mv -f /data/harmony/elite_remote_map.json /data/harmony/remote_slot1_map.json 2>/dev/null || true; "
                "[ -f /data/harmony/bt_devices.json ] && [ ! -f /data/harmony/bt-devices.json ] && mv -f /data/harmony/bt_devices.json /data/harmony/bt-devices.json 2>/dev/null || true; "
                "rm -rf /data/codex-backups /data/codexmqtt /data/harmony-backups /cache/crashlog-*.json 2>/dev/null; "
                "rm -f /data/harmony/mqtt_config.json /data/harmony/elite_remote_map.json /data/harmony/bt_devices.json /data/devices.json /data/activities.json /data/harmony/devices.json /data/harmony/config/.imported /data/remote_ui.ota.gz /data/remote_ui.version 2>/dev/null || true; "
                "rm -f /cache/codex_*.ota.gz /cache/codex_*.ota /cache/*.new /cache/*/*.new /data/harmony/*.new /data/harmony/*/*.new /data/rf_pair_test /data/test_hub_tx /data/codex_rf /data/hub_rf /data/harmony/bin/codex_* /cache/bin/codex_* /var/run/codex_* 2>/dev/null || true; "
                "mkdir -p /data/harmony/bin /data/harmony/www/assets /data/harmony/config /data/harmony/custom_icons"
            )
            if not self.dry_skip("move legacy /data/codex files to /data/harmony and remove obsolete files"):
                self.run_ssh(mig_paths_cmd)

            # Stock config conversion via migrate_hub_configs.py if needed
            has_dev_res = self.run_ssh("[ -f /data/harmony/config/devices.json ] && echo 1 || echo 0")
            has_dev = has_dev_res.stdout.strip() == "1"
            if not has_dev:
                has_stock_res = self.run_ssh("[ -f /data/resources/DeviceList.json ] && echo 1 || echo 0")
                has_stock = has_stock_res.stdout.strip() == "1"
                mig_script = None
                for cand in (
                    self.script_dir / "tools" / "migrate_hub_configs.py",
                    self.repo_root / "tools" / "migrate_hub_configs.py",
                ):
                    if cand.is_file():
                        mig_script = cand
                        break

                if has_stock and not mig_script:
                    print("[!] Stock configuration found but migrate_hub_configs.py missing: configs NOT migrated.")
                elif has_stock and mig_script and not self.dry_skip("migrate stock configuration and upload"):
                    print("[*] Migrating stock Logitech configurations to canonical v2.0 format...")
                    with tempfile.TemporaryDirectory() as tmp_dir:
                        tmp_p = pathlib.Path(tmp_dir)
                        # Pull /data/resources
                        scp_pull = ["scp", "-O", "-q", "-r", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no"]
                        if self.ssh_key and os.path.isfile(self.ssh_key):
                            scp_pull.extend(["-i", self.ssh_key])
                        scp_pull.extend([f"{self.hub_user}@{self.hub_host}:/data/resources", str(tmp_p / "resources")])
                        subprocess.run(scp_pull, capture_output=True)

                        out_dir = tmp_p / "out"
                        mig_res = subprocess.run([sys.executable, str(mig_script), "--src", str(tmp_p / "resources"), "--dest", str(out_dir)], capture_output=True, text=True)
                        if (out_dir / "config" / "devices.json").is_file():
                            scp_push = ["scp", "-O", "-q", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no"]
                            if self.ssh_key and os.path.isfile(self.ssh_key):
                                scp_push.extend(["-i", self.ssh_key])
                            # Push config/*.json
                            for c_file in (out_dir / "config").glob("*.json"):
                                subprocess.run(scp_push + [str(c_file), f"{self.hub_user}@{self.hub_host}:/data/harmony/config/"])
                            for m_file in out_dir.glob("*.json"):
                                subprocess.run(scp_push + [str(m_file), f"{self.hub_user}@{self.hub_host}:/data/harmony/"])
                            print("[+] Stock configurations converted and uploaded successfully.")
                        else:
                            print(f"[!] Stock configuration conversion failed: {mig_res.stderr.strip() or 'devices.json missing'}")
            elif self.dry_run:
                print("    [dry-run] /data/harmony/config/devices.json exists: stock config would not be imported")

            # Query remote hashes for core binaries and configs
            self.query_remote_hashes(
                "/data/harmony/bin/* /data/harmony/* /data/harmony/config/* /cache/www/* "
                "/cache/www/assets/* /cache/www/assets/themes/* /cache/modules/* /cache/bin/busybox "
                "/cache/icons.tar.gz /etc/init.d/rcS.local"
            )

            # Deploy core binaries
            if (self.bin_dir / "hub_util").is_file():
                self.deploy_file_if_changed(self.bin_dir / "hub_util", "/data/harmony/bin/hub_util", "755")
                util_setup = (
                    "for _app in hub_sync sync mknod register_ehci eth_fix hub_dhcpd hub_portal hub_sntp; do "
                    "  [ -f /data/harmony/bin/$_app ] && [ ! -L /data/harmony/bin/$_app ] && rm -f /data/harmony/bin/$_app 2>/dev/null; "
                    "  ln -sf /data/harmony/bin/hub_util /data/harmony/bin/$_app 2>/dev/null || true; "
                    "done"
                )
                if not self.dry_skip("create hub_util symlinks and purge legacy standalone binaries"):
                    self.run_ssh(util_setup)
                core_bins = ["hub_daemon", "hub_rf", "hub_btstack"]
            else:
                core_bins = [
                    "hub_daemon", "hub_rf", "hub_sntp", "hub_btstack", "hub_sync",
                    "hub_dhcpd", "hub_portal", "mknod", "register_ehci", "eth_fix"
                ]
            for b in core_bins:
                src = self.bin_dir / b
                if src.is_file():
                    self.deploy_file_if_changed(src, f"/data/harmony/bin/{b}", "755")

            # Extended BusyBox
            local_bb = self.bin_dir / "busybox"
            if local_bb.is_file():
                print("[*] Syncing extended BusyBox to /cache/bin/...")
                if not self.dry_run:
                    self.run_ssh("mkdir -p /cache/bin 2>/dev/null")
                self.deploy_file_if_changed(local_bb, "/cache/bin/busybox", "755")
                bb_setup = (
                    "ln -sf /cache/bin/busybox /data/harmony/bin/busybox 2>/dev/null; "
                    "/cache/bin/busybox --install -s /cache/bin 2>/dev/null || true; "
                    'for _app in $(/cache/bin/busybox --list 2>/dev/null); do [ ! -e "/bin/$_app" ] && [ ! -e "/usr/bin/$_app" ] && ln -sf /cache/bin/busybox "/usr/bin/$_app" 2>/dev/null || true; done; '
                    "mkdir -p /etc/profile.d 2>/dev/null; echo 'export PATH=/cache/bin:/data/harmony/bin:$PATH' > /etc/profile.d/00-harmony.sh 2>/dev/null; chmod 644 /etc/profile.d/00-harmony.sh 2>/dev/null || true"
                )
                if not self.dry_skip("install BusyBox applet links and /etc/profile.d/00-harmony.sh"):
                    self.run_ssh(bb_setup)

            # Ensure root profiles
            prof_cmd = (
                "for prof in /root/.profile /home/root/.profile; do mkdir -p \"$(dirname \"$prof\")\" 2>/dev/null; "
                'printf "BB_PATH=\\"\\"\n[ -x /cache/bin/busybox ] && BB_PATH=/cache/bin/busybox\n'
                "[ -x /cache/busybox ] && BB_PATH=/cache/busybox\n"
                'if [ -t 0 ] && [ -n \\"\\$BB_PATH\\" ] && [ -z \\"\\$BB_EXTENDED\\" ]; then\n'
                "  export BB_EXTENDED=1\n  export PATH=/cache/bin:/data/harmony/bin:\\$PATH\n"
                '  exec \\"\\$BB_PATH\\" sh -l\nfi\nexport PATH=/cache/bin:/data/harmony/bin:\\$PATH\n" > "$prof"; '
                'chmod 644 "$prof" 2>/dev/null || true; done'
            )
            if not self.dry_skip("rewrite /root/.profile and /home/root/.profile"):
                self.run_ssh(prof_cmd)

            # Deploy payload scripts
            if self.scripts_dir.is_dir():
                print("[*] Syncing startup & network scripts to /data/harmony/...")
                for sc in self.scripts_dir.iterdir():
                    if sc.is_file():
                        self.deploy_file_if_changed(sc, f"/data/harmony/{sc.name}", "755")

            # Deploy VERSION
            ver_file = self.repo_root / "VERSION"
            if ver_file.is_file():
                print("[*] Syncing VERSION file to /data/harmony/VERSION...")
                self.deploy_file_if_changed(ver_file, "/data/harmony/VERSION", "644")

            # Remote OTA: Do not deploy remote_ui.ota.gz to flash storage on Hub.
            # Only record remote version metadata and clean up any legacy flash OTA binaries.
            print("[*] Recording remote version metadata and cleaning flash OTA cache...")
            if not self.dry_skip("clean flash OTA images and sync remote_ui.version"):
                self.run_ssh("rm -f /data/harmony/remote_ui.ota.gz /cache/remote_ui.ota.gz "
                             "/data/harmony/elite_ui.ota.gz /cache/elite_ui.ota.gz /data/harmony/elite_ui.version 2>/dev/null || true; "
                             "cmp -s /data/harmony/VERSION /data/harmony/remote_ui.version || "
                             "cp -f /data/harmony/VERSION /data/harmony/remote_ui.version 2>/dev/null || true")

            # Stage local remote OTA image into /tmp (RAM only, no flash wear) if requested
            if self.stage_remote_ota:
                candidates = [
                    self.repo_root / "remote" / "bin" / "remote_ui.ota.gz",
                    self.bin_dir / "remote_ui.ota.gz",
                    self.repo_root / "remote" / "remote_ui.ota.gz",
                ]
                staged = False
                for cand in candidates:
                    if cand.is_file():
                        print(f"[*] Staging local {cand} to Hub /tmp/remote_ui.ota.gz (RAM tmpfs)...")
                        if not self.dry_run:
                            self.deploy_file_if_changed(cand, "/tmp/remote_ui.ota.gz", "644")
                            self.run_ssh("rm -f /tmp/remote_manifest.json 2>/dev/null || true")
                        staged = True
                        break
                if not staged:
                    print("[-] Warning: --stage-remote-ota requested but no remote_ui.ota.gz found locally", file=sys.stderr)
            else:
                if not self.dry_run:
                    self.run_ssh("rm -f /tmp/remote_ui.ota.gz /tmp/remote_manifest.json 2>/dev/null || true")

            # Deploy configs
            if self.config_dir.is_dir():
                print("[*] Syncing config files to /data/harmony/config/...")
                if not self.dry_run:
                    self.run_ssh("mkdir -p /data/harmony/config 2>/dev/null || true")
                for cfg in self.config_dir.iterdir():
                    if cfg.is_file():
                        self.deploy_file_if_changed(cfg, f"/data/harmony/config/{cfg.name}", "644")
                        if cfg.name == "cacert.pem" and not self.dry_run:
                            self.run_ssh("ln -sf /data/harmony/config/cacert.pem /data/harmony/cacert.pem 2>/dev/null || true")

            # Deploy USB Ethernet kernel modules
            if self.modules_dir.is_dir():
                print("[*] Syncing USB Ethernet kernel modules to /cache/modules/...")
                if not self.dry_run:
                    self.run_ssh("mkdir -p /cache/modules 2>/dev/null; [ -L /data/harmony/modules ] || ln -sf /cache/modules /data/harmony/modules")
                for mod in self.modules_dir.glob("*.ko"):
                    self.deploy_file_if_changed(mod, f"/cache/modules/{mod.name}", "644")

            # Deploy startup script rcS.local
            rcs_local = self.scripts_dir / "rcS.local"
            if rcs_local.is_file():
                print("[*] Installing startup script /etc/init.d/rcS.local...")
                self.deploy_file_if_changed(rcs_local, "/etc/init.d/rcS.local", "755")
        else:
            print("[*] WebUI-only deployment mode active (--webui-only).")
            self.query_remote_hashes("/cache/www/* /cache/www/assets/* /cache/www/assets/themes/* /cache/icons.tar.gz")

        # 4. Deploy WebUI to /cache/www/
        print(f"[4/5] Building & syncing WebUI ({self.ui_choice}) to /cache/www/ (MD5 check)...")
        # Build WebUI if in source repo
        if self.www_dir == (self.repo_root / "payload" / "www") and (self.repo_root / "frontend").is_dir():
            if self.dry_run:
                print(f"    [dry-run] would build web UI ({self.ui_choice}) first; comparison below uses files currently in payload/www")
            else:
                ui_src_dir = self.repo_root / ("android-app" if self.ui_choice == "legacy" else "frontend")
                if shutil.which("emcc"):
                    render_sh = self.repo_root / "tools" / "remote_render" / "build.sh"
                    if render_sh.is_file():
                        subprocess.run(["bash", str(render_sh), "--wasm", str(ui_src_dir / "public" / "assets")], capture_output=True)
                    theme_py = self.repo_root / "tools" / "remote_render" / "build_theme.py"
                    if theme_py.is_file():
                        subprocess.run([sys.executable, str(theme_py), "--tar", str(ui_src_dir / "public" / "assets" / "themes")], capture_output=True)

                print(f"[*] Compiling frontend bundle with Vite ({ui_src_dir.name})...")
                npm_cmd = "npm.cmd" if sys.platform == "win32" else "npm"
                build_res = subprocess.run([npm_cmd, "run", "build"], cwd=ui_src_dir)
                if build_res.returncode != 0 or not (ui_src_dir / "dist" / "index.html").is_file():
                    print(f"[-] WebUI build failed in {ui_src_dir.name}. Nothing deployed from this step.", file=sys.stderr)
                    return False

                self.www_dir.mkdir(parents=True, exist_ok=True)
                for p in self.www_dir.iterdir():
                    if p.is_file():
                        p.unlink(missing_ok=True)
                assets_d = self.www_dir / "assets"
                if assets_d.is_dir():
                    shutil.rmtree(assets_d, ignore_errors=True)

                dist_d = ui_src_dir / "dist"
                for item in dist_d.iterdir():
                    dest = self.www_dir / item.name
                    if item.is_dir():
                        shutil.copytree(item, dest, dirs_exist_ok=True)
                    else:
                        shutil.copy2(item, dest)

                icons_sub = assets_d / "icons"
                if icons_sub.is_dir():
                    shutil.rmtree(icons_sub, ignore_errors=True)

                gzip_script = self.repo_root / "build" / "gzip_assets.py"
                if gzip_script.is_file():
                    subprocess.run([sys.executable, str(gzip_script), str(self.www_dir)], capture_output=True)

        if not self.dry_run:
            self.run_ssh(
                "mkdir -p /cache/www/assets 2>/dev/null; rm -rf /cache/www/assets/icons 2>/dev/null; "
                "if [ -d /data/harmony/www ] && [ ! -L /data/harmony/www ]; then rm -rf /data/harmony/www 2>/dev/null; fi; "
                "mkdir -p /data/harmony 2>/dev/null; [ -L /data/harmony/www ] || ln -sfn /cache/www /data/harmony/www 2>/dev/null"
            )

        if self.dry_run and not self.www_dir.is_dir():
            print(f"    [dry-run] {self.www_dir} does not exist: web UI files not compared")
        else:
            # Purge obsolete assets on Hub
            remote_assets_res = self.run_ssh("for f in /cache/www/assets/*; do [ -f \"$f\" ] && echo \"${f##*/}\"; done; true")
            for r_file in remote_assets_res.stdout.splitlines():
                r_file = r_file.strip()
                if r_file and not (self.www_dir / "assets" / r_file).is_file():
                    if self.dry_run:
                        print(f"    -> Would purge obsolete asset: {r_file}")
                    else:
                        print(f"    [-] Purging obsolete asset: {r_file}")
                        self.run_ssh(f"rm -f '/cache/www/assets/{r_file}' 2>/dev/null || true")

            # Deploy WebUI files
            for root, _, files in os.walk(self.www_dir):
                for f in files:
                    wfile = pathlib.Path(root) / f
                    rel = wfile.relative_to(self.www_dir).as_posix()
                    self.deploy_file_if_changed(wfile, f"/cache/www/{rel}", "644")

        # Icon archive
        if not self.icons_tar.is_file() and self.dry_run:
            print(f"    [dry-run] {self.icons_tar.name} not found: deployment would offer to build it; Hub icons left as-is")
        elif not self.icons_tar.is_file() and self.fw_tool.is_file():
            print(f"\n[?] {self.icons_tar.name} not found on host.")
            try:
                reply = input(f"Download official remote firmware, extract 1,350 icons, and build {self.icons_tar.name} now? [Y/n] ")
            except EOFError:
                reply = "y"
            if not reply.strip() or reply.strip().lower().startswith("y"):
                print(f"[*] Extracting remote UI icons and packaging {self.icons_tar.name}...")
                subprocess.run([sys.executable, str(self.fw_tool), "extract-icons", "--archive", str(self.icons_tar)])

            if not self.icons_tar.is_file():
                print("[!] No icon archive: Hub icons left as-is.")

        if self.icons_tar.is_file():
            self.deploy_file_if_changed(self.icons_tar, "/cache/icons.tar.gz", "644")
            if not self.dry_run:
                self.run_ssh(
                    "if [ -f /data/harmony/icons.tar.gz ] && [ ! -L /data/harmony/icons.tar.gz ]; then rm -f /data/harmony/icons.tar.gz 2>/dev/null; fi; "
                    "[ -L /data/harmony/icons.tar.gz ] || ln -sf /cache/icons.tar.gz /data/harmony/icons.tar.gz 2>/dev/null"
                )

        if not self.dry_run:
            self.run_ssh("chmod 755 /cache/www /cache/www/assets 2>/dev/null || true")

        # 5. Safe reboot OR hot service restart
        if self.dry_run:
            action = "reboot the Hub (safe_reboot.sh)" if self.reboot_hub else "restart hub services (init.sh start)"
            print(f"[5/5] [dry-run] would {action}")
            print(f"=== Dry run for {self.hub_host} complete: nothing was changed. ===")
        elif self.reboot_hub:
            print("[5/5] Verifying system shell and flushing NOR flash buffers (rebooting)...")
            reboot_cmd = (
                'if ! /bin/sh -c "true" 2>/dev/null; then echo "[-] CRITICAL: /bin/sh verification failed. Aborting reboot!"; exit 1; fi; '
                "chmod +x /data/harmony/safe_reboot.sh 2>/dev/null || true; "
                "ln -sf /data/harmony/safe_reboot.sh /data/harmony/bin/safe_reboot.sh 2>/dev/null || true; "
                "ln -sf /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true; "
                "(sleep 2 && /data/harmony/safe_reboot.sh) >/dev/null 2>&1 &"
            )
            self.run_ssh(reboot_cmd)
            print(f"=== Deployment to {self.hub_host} Complete! Hub is rebooting safely. ===")
        else:
            print("[5/5] Flushing NOR flash buffers and hot-restarting services (no reboot)...")
            restart_cmd = (
                "chmod +x /data/harmony/bin/* /data/harmony/*.sh /data/harmony/init.sh 2>/dev/null || true; "
                "mkdir -p /data/harmony/scripts 2>/dev/null || true; "
                "ln -sf /data/harmony/init.sh /data/harmony/scripts/init.sh 2>/dev/null || true; "
                "ln -sf /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true; "
                "ln -sf /data/harmony/bin/hub_sync /cache/bin/sync 2>/dev/null || true; "
                "/data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true; "
                "rm -f /tmp/harmony_stop 2>/dev/null || true; "
                "if [ -x /data/harmony/init.sh ]; then /data/harmony/init.sh restart >/dev/null 2>&1 & fi"
            )
            self.run_ssh(restart_cmd)
            time.sleep(3)
            procs_res = self.run_ssh("pidof hub_daemon hub_rf hub_btstack 2>/dev/null || true")
            procs = procs_res.stdout.strip()
            if procs:
                print(f"=== Deployment to {self.hub_host} Complete! Hub services running (PIDs: {procs}). ===")
            else:
                print(f"=== Deployment to {self.hub_host} Complete! Services started. ===")

        if self.deploy_failures:
            print("[-] These files were not deployed; Hub runs with old copies:", file=sys.stderr)
            for f in self.deploy_failures:
                print(f"    {f}", file=sys.stderr)
            return False

        return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Deploy firmware, WebUI, and configuration to Logitech Harmony Hub")
    parser.add_argument("host", nargs="?", help="Harmony Hub IP or hostname")
    parser.add_argument("--host", "-H", dest="flag_host", help="Harmony Hub IP or hostname (alternative)")
    parser.add_argument("--user", default="root", help="SSH username (default: root)")
    parser.add_argument("--key", "-k", help="Path to SSH private key")
    parser.add_argument(
        "--ui",
        choices=["legacy", "new", "v3", "frontend", "old", "android-app"],
        default="legacy",
        help="Choice of Web UI: 'legacy' (android-app) or 'new' (frontend v3) (default: legacy)",
    )
    parser.add_argument("--ui-legacy", "--legacy-ui", "--old-ui", action="store_true", help="Alias for --ui=legacy")
    parser.add_argument("--ui-new", "--new-ui", action="store_true", help="Alias for --ui=new")
    parser.add_argument("--webui-only", "--ui-only", action="store_true", help="Deploy Web UI assets only to /cache/www")
    parser.add_argument("--reboot", action="store_true", help="Reboot Hub safely after deployment (default: hot restart)")
    parser.add_argument("--dry-run", "-n", action="store_true", help="Read Hub state and report changes without modifying Hub")
    parser.add_argument("--no-backup", action="store_true", help="Skip pre-deployment backup")
    parser.add_argument("--stage-remote-ota", action="store_true", help="Stage local remote_ui.ota.gz into Hub /tmp/remote_ui.ota.gz (RAM tmpfs) for offline testing")

    args = parser.parse_args()
    target_host = args.flag_host or args.host
    if not target_host:
        parser.error("HUB_ADDRESS is required (pass as first argument or --host=IP)")

    ui_choice = "legacy"
    if args.ui_new:
        ui_choice = "new"
    elif args.ui_legacy:
        ui_choice = "legacy"
    else:
        ui_choice = args.ui

    deployer = HubDeployer(
        hub_host=target_host,
        hub_user=args.user,
        ssh_key=args.key,
        ui_choice=ui_choice,
        webui_only=args.webui_only,
        reboot_hub=args.reboot,
        dry_run=args.dry_run,
        no_backup=args.no_backup,
        stage_remote_ota=args.stage_remote_ota,
    )
    success = deployer.deploy()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
