#!/usr/bin/env python3
"""
Streaming tar sync utility: Extract activities, devices, and icons from the Harmony remote
and stream directly to Harmony Hub using extended BusyBox tar & gzip.

Key principles:
1. Old icons removed from Hub /data to free flash space.
2. Icons stored in /cache/www/assets/icons and symlinked from /data/harmony/www/assets/icons.
3. NEVER touch or delete stock /data/resources/ or *List.json files on Hub or remote.
4. Checks /data/harmony/config/.imported touch file to avoid duplicate reimports
   (--force imports again).

Note: the current Hub firmware unpacks its icons from icons.tar.gz into /tmp at every
boot (init.sh removes /cache/www/assets/icons), and imports a stock configuration
through build/deploy_hub.py; nothing on the Hub reads /cache/remote_transfer or
/cache/stock_import. The tool says so when it runs.
"""

import argparse
import os
import subprocess
import sys
import time

DEFAULT_REMOTE_IP = "192.168.2.1"
DEFAULT_HUB_IP = "192.168.1.1"


def run_ssh(host, cmd, timeout=30):
    """Run an SSH command on a remote device."""
    args = [
        "ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
        "-o", "ConnectTimeout=5", f"root@{host}", cmd
    ]
    try:
        res = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return res.returncode == 0, res.stdout.decode("utf-8", errors="replace"), res.stderr.decode("utf-8", errors="replace")
    except Exception as e:
        return False, "", str(e)


def ssh_args(host):
    return ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=5", f"root@{host}"]


def run_pipe(src_host, src_cmd, dst_host, dst_cmd, timeout=600):
    """Stream src_cmd's output on one device into dst_cmd on the other. True only when
    both ends (and both ssh logins) succeeded; otherwise also the error text."""
    p1 = subprocess.Popen(ssh_args(src_host) + [src_cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    p2 = subprocess.Popen(ssh_args(dst_host) + [dst_cmd], stdin=p1.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    p1.stdout.close()
    try:
        _, err2 = p2.communicate(timeout=timeout)
        err1 = p1.stderr.read()
        p1.wait(timeout=30)
    except subprocess.TimeoutExpired:
        p1.kill()
        p2.kill()
        return False, "timed out"
    finally:
        p1.stderr.close()
    if p1.returncode != 0 or p2.returncode != 0:
        msg = (err1 + err2).decode("utf-8", errors="replace").strip()
        return False, f"source exit {p1.returncode}, target exit {p2.returncode}{': ' + msg if msg else ''}"
    return True, ""


def get_hub_space(hub_ip):
    """Query /data and /cache usage on Hub."""
    ok, out, _ = run_ssh(hub_ip, "/cache/bin/busybox df -h /cache /mnt/data 2>/dev/null || df -h /cache /data 2>/dev/null")
    if ok and out:
        return out.strip()
    return "Unknown"


def main():
    parser = argparse.ArgumentParser(description="Stream sync remote assets and configs to Hub via tar.")
    parser.add_argument("--remote", default=DEFAULT_REMOTE_IP, help=f"Remote IP (default: {DEFAULT_REMOTE_IP})")
    parser.add_argument("--hub", default=DEFAULT_HUB_IP, help=f"Harmony Hub IP (default: {DEFAULT_HUB_IP})")
    parser.add_argument("--force", action="store_true", help="Force reimport even if .imported flag exists")
    parser.add_argument("--skip-icons", action="store_true", help="Skip icon sync")
    parser.add_argument("--skip-config", action="store_true", help="Skip config sync")
    args = parser.parse_args()

    print("==================================================")
    print("  Remote -> Hub Streaming Tar Sync Utility        ")
    print("==================================================")
    print(f"Source (remote): {args.remote}")
    print(f"Target (Hub):   {args.hub}")
    print("--------------------------------------------------")

    # 1. Test connectivity
    print("[*] Probing remote SSH...")
    ok_remote, remote_uname, err_remote = run_ssh(args.remote, "uname -m")
    if not ok_remote:
        print(f"[!] Error: Cannot connect to the remote at {args.remote}. Ensure USB is connected.")
        sys.exit(1)
    if not remote_uname.strip().startswith("arm"):
        print(f"[!] Error: {args.remote} is not a Harmony remote (uname -m: {remote_uname.strip()}).")
        sys.exit(1)
    print(f"[+] Remote reachable ({remote_uname.strip()})")

    print("[*] Probing Hub SSH...")
    ok_hub, hub_uname, err_hub = run_ssh(args.hub, "uname -m")
    if not ok_hub:
        print(f"[!] Error: Cannot connect to Hub at {args.hub}.")
        sys.exit(1)
    if not hub_uname.strip().startswith("mips"):
        print(f"[!] Error: {args.hub} is not a Harmony Hub (uname -m: {hub_uname.strip()}).")
        sys.exit(1)
    print(f"[+] Hub reachable ({hub_uname.strip()})")
    print("[!] Note: the current Hub firmware does not read what this tool copies (see the tool's description);")
    print("    the icons are removed again at the Hub's next boot.")
    failures = []

    # 2. Check initial Hub flash space
    print("\n[*] Initial Hub filesystem usage:")
    print(get_hub_space(args.hub))

    # 3. Check import flag on Hub
    check_flag_cmd = "[ -f /data/harmony/config/.imported ] && echo 'imported' || echo 'not_imported'"
    ok_flag, flag_out, _ = run_ssh(args.hub, check_flag_cmd)
    already_imported = ok_flag and flag_out.strip() == "imported"
    print(f"[*] Hub import flag status: {'ALREADY IMPORTED' if already_imported else 'NOT YET IMPORTED'}")
    if already_imported and not args.force:
        print("[+] Nothing to do: the Hub already imported a configuration (use --force to import again).")
        sys.exit(0)

    # 4. Remove old icons from /data on Hub to reclaim NAND space. /data/harmony/www can be a
    # link to /cache/www: then /data/harmony/www/assets/icons is the target folder itself and
    # is left alone (removing it and linking it to itself would create icons/icons).
    if not args.skip_icons:
        print("\n[*] Cleaning old icon assets from Hub /data partition...")
        clean_cmd = (
            "mkdir -p /cache/www/assets/icons; "
            "if [ -L /data/harmony/www ]; then echo linked; exit 0; fi; "
            "rm -rf /data/harmony/www/assets/icons && mkdir -p /data/harmony/www/assets && "
            "ln -sfn /cache/www/assets/icons /data/harmony/www/assets/icons"
        )
        ok_clean, clean_out, err_clean = run_ssh(args.hub, clean_cmd)
        if not ok_clean:
            print(f"[!] Error cleaning icons on Hub: {err_clean.strip()}")
            failures.append("icon folder")
        elif clean_out.strip() == "linked":
            print("[+] /data/harmony/www is a link to /cache/www: icon folder left as it is")
        else:
            print("[+] Replaced /data/harmony/www/assets/icons with symlink -> /cache/www/assets/icons")

    # 5. Stream icons from the remote to Hub via tar pipe
    if not args.skip_icons and not failures:
        print("\n[*] Streaming icons from remote -> Hub via tar & gzip pipeline...")
        # Check available icon directories on the remote
        subdirs = "commands footer header home hopsactivity juniperactivity hopsDevice juniperDevice hopsHomeAutomation juniperHomeAutomation notification settings"
        remote_tar_cmd = (
            f"if [ -x /data/busybox ]; then "
            f"  /data/busybox tar -cf - -C /opt/luadraws/appscommon/asset {subdirs} 2>/dev/null | /data/busybox gzip -c; "
            f"else "
            f"  tar -czf - -C /opt/luadraws/appscommon/asset {subdirs} 2>/dev/null; "
            f"fi"
        )
        hub_untar_cmd = (
            "mkdir -p /cache/www/assets/icons && "
            "if [ -x /cache/bin/busybox ]; then "
            "  /cache/bin/busybox tar -xzf - -C /cache/www/assets/icons; "
            "else "
            "  tar -xzf - -C /cache/www/assets/icons; "
            "fi"
        )

        t0 = time.time()
        ok_icons, err_icons = run_pipe(args.remote, remote_tar_cmd, args.hub, hub_untar_cmd)
        elapsed = time.time() - t0

        if ok_icons:
            print(f"[+] Icon stream extracted successfully in {elapsed:.2f}s!")
            # Flatten copies for root icon lookups (e.g. /assets/icons/116.png)
            print("[*] Creating flat icon index in /cache/www/assets/icons/...")
            flatten_cmd = (
                "cd /cache/www/assets/icons && "
                "for f in */*.png; do "
                "  [ -f \"$f\" ] || continue; "
                "  base=\"${f##*/}\"; "
                "  [ ! -e \"$base\" ] && ln -sf \"$f\" \"$base\" 2>/dev/null || true; "
                "done; "
                "[ -e juniperDevice/1.png ] && [ ! -e tv.png ] && ln -sf juniperDevice/1.png tv.png 2>/dev/null; "
                "[ -e notification/tv.png ] && ln -sf notification/tv.png tv.png 2>/dev/null; "
                "[ -e juniperDevice/5.png ] && [ ! -e receiver.png ] && ln -sf juniperDevice/5.png receiver.png 2>/dev/null; "
                "[ -e juniperDevice/default.png ] && [ ! -e generic.png ] && ln -sf juniperDevice/default.png generic.png 2>/dev/null; "
                "[ -e commands/116.png ] && [ ! -e poweroff.png ] && ln -sf commands/116.png poweroff.png 2>/dev/null; "
                "[ -e notification/switch.png ] && [ ! -e switch.png ] && ln -sf notification/switch.png switch.png 2>/dev/null; "
                "[ -e juniperHomeAutomation/bulb_on.png ] && [ ! -e light.png ] && ln -sf juniperHomeAutomation/bulb_on.png light.png 2>/dev/null; "
                "ls -1 *.png 2>/dev/null | wc -l"
            )
            _, count_out, _ = run_ssh(args.hub, flatten_cmd)
            cnt = count_out.strip() if count_out else "multiple"
            print(f"[+] Total icons ready on Hub: {cnt}")
        else:
            print(f"[!] Error streaming icons: {err_icons}")
            failures.append("icons")

    # 6. Stream activities & devices from the remote to Hub
    if not args.skip_config:
        print("\n[*] Checking activities and devices on the remote...")
        # Check if the remote has remote_ui.conf
        ok_cfg, cfg_content, _ = run_ssh(args.remote, "cat /data/remote_ui.conf 2>/dev/null || true")
        if ok_cfg and cfg_content.strip().startswith("{"):
            print("[+] Found active config on the remote!")
            # Save to /cache/remote_transfer/ on Hub
            remote_save_cmd = (
                "mkdir -p /cache/remote_transfer; "
                "cat > /cache/remote_transfer/remote_ui.conf"
            )
            p_save = subprocess.run(ssh_args(args.hub) + [remote_save_cmd], input=cfg_content.encode("utf-8"),
                                    capture_output=True, timeout=60)
            if p_save.returncode == 0:
                print("[+] Saved remote config to Hub: /cache/remote_transfer/remote_ui.conf")
            else:
                print(f"[!] Error saving the remote config on the Hub: {p_save.stderr.decode(errors='replace').strip()}")
                failures.append("remote config")

        # Stream stock resources from the remote if non-empty
        print("[*] Checking stock resources on the remote...")
        chk_res = "[ -s /data/resources/ActivityList.json ] && echo 'has_resources' || echo 'empty'"
        _, res_status, _ = run_ssh(args.remote, chk_res)
        if "has_resources" in res_status:
            print("[+] Found non-empty stock resources on the remote. Streaming to /cache/stock_import on Hub...")
            # Stock BusyBox on the remote has no tar: the project's /data/busybox is needed
            tar_res_cmd = ("[ -x /data/busybox ] || { echo 'no /data/busybox on the remote' >&2; exit 1; }; "
                           "/data/busybox tar -cf - -C / data/resources | /data/busybox gzip -c")
            hub_untar_res = "mkdir -p /cache/stock_import && /cache/bin/busybox tar -xzf - -C /cache/stock_import"
            ok_res, err_res = run_pipe(args.remote, tar_res_cmd, args.hub, hub_untar_res)
            if ok_res:
                print("[+] Extracted stock resources to Hub: /cache/stock_import/data/resources/")
            else:
                print(f"[!] Error streaming stock resources: {err_res}")
                failures.append("stock resources")
        else:
            print("[*] Stock resources on the remote are empty or already migrated.")

    # 7. Check / set import flag on Hub
    print("\n[*] Verifying /data/harmony/config/ on Hub...")
    verify_hub_cfg = (
        "FLAG=''; "
        "if [ -f /data/harmony/config/.imported ]; then "
        "  echo 'flag_exists'; "
        "elif [ -s /data/harmony/config/devices.json ] && [ -s /data/harmony/config/activities.json ]; then "
        "  mkdir -p /data/harmony/config; "
        "  touch /data/harmony/config/.imported 2>/dev/null || true; "
        "  echo 'flag_created'; "
        "else "
        "  echo 'config_missing'; "
        "fi"
    )
    _, flag_res, _ = run_ssh(args.hub, verify_hub_cfg)
    flag_res = flag_res.strip()
    if "flag_created" in flag_res:
        print("[+] Created touch file /data/harmony/config/.imported (future reimports disabled).")
    elif "flag_exists" in flag_res:
        print("[+] Touch file /data/harmony/config/.imported is verified active.")
    else:
        print("[!] Note: /data/harmony/config/ missing devices or activities. Run hub_daemon to complete first-boot migration.")

    # 8. Check final Hub flash space
    print("\n[*] Final Hub filesystem usage:")
    print(get_hub_space(args.hub))
    if failures:
        print(f"\n[-] Remote -> Hub sync INCOMPLETE, failed: {', '.join(failures)}")
        sys.exit(1)
    print("\n[+] Remote -> Hub sync complete!")


if __name__ == "__main__":
    if sys.version_info < (3, 11):
        sys.exit(f"This tool needs Python 3.11 or newer (this is Python {sys.version.split()[0]}).")
    main()
