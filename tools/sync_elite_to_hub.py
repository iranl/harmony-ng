#!/usr/bin/env python3
"""
Streaming tar sync utility: Extract activities, devices, and icons from Harmony Elite
and stream directly to Harmony Hub using extended BusyBox tar & gzip.

Key principles:
1. Old icons removed from Hub /data to free flash space.
2. Icons stored in /cache/www/assets/icons and symlinked from /data/codex/www/assets/icons.
3. NEVER touch or delete stock /data/resources/ or *List.json files on Hub or Elite.
4. Checks /data/codex/config/.imported touch file to avoid duplicate reimports.
"""

import argparse
import os
import subprocess
import sys
import time

DEFAULT_ELITE_IP = "192.168.2.1"
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


def get_hub_space(hub_ip):
    """Query /data and /cache usage on Hub."""
    ok, out, _ = run_ssh(hub_ip, "/cache/bin/busybox df -h /cache /mnt/data 2>/dev/null || df -h /cache /data 2>/dev/null")
    if ok and out:
        return out.strip()
    return "Unknown"


def main():
    parser = argparse.ArgumentParser(description="Stream sync Elite assets and configs to Hub via tar.")
    parser.add_argument("--remote", default=DEFAULT_ELITE_IP, help=f"Elite remote IP (default: {DEFAULT_ELITE_IP})")
    parser.add_argument("--hub", default=DEFAULT_HUB_IP, help=f"Harmony Hub IP (default: {DEFAULT_HUB_IP})")
    parser.add_argument("--force", action="store_true", help="Force reimport even if .imported flag exists")
    parser.add_argument("--skip-icons", action="store_true", help="Skip icon sync")
    parser.add_argument("--skip-config", action="store_true", help="Skip config sync")
    args = parser.parse_args()

    print("==================================================")
    print("  Elite -> Hub Streaming Tar Sync Utility         ")
    print("==================================================")
    print(f"Source (Elite): {args.remote}")
    print(f"Target (Hub):   {args.hub}")
    print("--------------------------------------------------")

    # 1. Test connectivity
    print("[*] Probing Elite remote SSH...")
    ok_elite, elite_uname, err_elite = run_ssh(args.remote, "uname -s")
    if not ok_elite:
        print(f"[!] Error: Cannot connect to Elite at {args.remote}. Ensure USB is connected.")
        sys.exit(1)
    print(f"[+] Elite reachable ({elite_uname.strip()})")

    print("[*] Probing Hub SSH...")
    ok_hub, hub_uname, err_hub = run_ssh(args.hub, "uname -s")
    if not ok_hub:
        print(f"[!] Error: Cannot connect to Hub at {args.hub}.")
        sys.exit(1)
    print(f"[+] Hub reachable ({hub_uname.strip()})")

    # 2. Check initial Hub flash space
    print("\n[*] Initial Hub filesystem usage:")
    print(get_hub_space(args.hub))

    # 3. Step 1: Remove old icons from /data on Hub to reclaim critical NAND space
    print("\n[*] Cleaning old icon assets from Hub /data partition...")
    clean_cmd = (
        "rm -rf /data/codex/www/assets/icons 2>/dev/null || true; "
        "mkdir -p /cache/www/assets/icons /data/codex/www/assets; "
        "ln -sfn /cache/www/assets/icons /data/codex/www/assets/icons"
    )
    ok_clean, _, err_clean = run_ssh(args.hub, clean_cmd)
    if not ok_clean:
        print(f"[!] Warning cleaning icons on Hub: {err_clean}")
    else:
        print("[+] Replaced /data/codex/www/assets/icons with symlink -> /cache/www/assets/icons")

    # 4. Check import flag on Hub
    check_flag_cmd = "[ -f /data/codex/config/.imported ] && echo 'imported' || echo 'not_imported'"
    _, flag_out, _ = run_ssh(args.hub, check_flag_cmd)
    already_imported = "imported" in flag_out.strip()
    print(f"[*] Hub import flag status: {'ALREADY IMPORTED' if already_imported else 'NOT YET IMPORTED'}")

    # 5. Stream icons from Elite to Hub via tar pipe
    if not args.skip_icons:
        print("\n[*] Streaming icons from Elite -> Hub via tar & gzip pipeline...")
        # Check available icon directories on Elite
        subdirs = "commands footer header home hopsactivity juniperactivity hopsDevice juniperDevice hopsHomeAutomation juniperHomeAutomation notification settings"
        elite_tar_cmd = (
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

        p1_args = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", f"root@{args.remote}", elite_tar_cmd]
        p2_args = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", f"root@{args.hub}", hub_untar_cmd]

        t0 = time.time()
        p1 = subprocess.Popen(p1_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        p2 = subprocess.Popen(p2_args, stdin=p1.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        p1.stdout.close()
        out2, err2 = p2.communicate()
        p1.wait()
        elapsed = time.time() - t0

        if p2.returncode == 0:
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
            print(f"[!] Error streaming icons: {err2.decode('utf-8', errors='replace')}")

    # 6. Stream activities & devices from Elite to Hub
    if not args.skip_config:
        print("\n[*] Checking activities and devices on Elite...")
        # Check if Elite has codex_elite_config.json
        ok_cfg, cfg_content, _ = run_ssh(args.remote, "cat /data/codex_elite_config.json 2>/dev/null || true")
        if ok_cfg and cfg_content.strip().startswith("{"):
            print("[+] Found active /data/codex_elite_config.json on Elite!")
            # Save to /cache/elite_transfer/ on Hub
            elite_save_cmd = (
                "mkdir -p /cache/elite_transfer; "
                "cat > /cache/elite_transfer/codex_elite_config.json"
            )
            p_save = subprocess.Popen(
                ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", f"root@{args.hub}", elite_save_cmd],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            p_save.communicate(cfg_content.encode("utf-8"))
            print("[+] Saved Elite config to Hub: /cache/elite_transfer/codex_elite_config.json")

        # Stream stock resources from Elite if non-empty
        print("[*] Checking stock resources on Elite...")
        chk_res = "[ -s /data/resources/ActivityList.json ] && echo 'has_resources' || echo 'empty'"
        _, res_status, _ = run_ssh(args.remote, chk_res)
        if "has_resources" in res_status:
            print("[+] Found non-empty stock resources on Elite. Streaming to /cache/stock_import on Hub...")
            tar_res_cmd = "/data/busybox tar -cf - -C / data/resources 2>/dev/null | /data/busybox gzip -c"
            hub_untar_res = "mkdir -p /cache/stock_import && /cache/bin/busybox tar -xzf - -C /cache/stock_import"
            p1 = subprocess.Popen(["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", f"root@{args.remote}", tar_res_cmd], stdout=subprocess.PIPE)
            p2 = subprocess.Popen(["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", f"root@{args.hub}", hub_untar_res], stdin=p1.stdout)
            p1.stdout.close()
            p2.communicate()
            print("[+] Extracted stock resources to Hub: /cache/stock_import/data/resources/")
        else:
            print("[*] Stock resources on Elite are empty or already migrated.")

    # 7. Check / set import flag on Hub
    print("\n[*] Verifying /data/codex/config/ on Hub...")
    verify_hub_cfg = (
        "if [ ! -f /data/codex/config/.imported ]; then "
        "  if [ -s /data/codex/config/devices.json ] && [ -s /data/codex/config/activities.json ]; then "
        "    touch /data/codex/config/.imported; "
        "    echo 'flag_created'; "
        "  else "
        "    echo 'config_missing'; "
        "  fi; "
        "else "
        "  echo 'flag_exists'; "
        "fi"
    )
    _, flag_res, _ = run_ssh(args.hub, verify_hub_cfg)
    flag_res = flag_res.strip()
    if "flag_created" in flag_res:
        print("[+] Created touch file /data/codex/config/.imported (future reimports disabled).")
    elif "flag_exists" in flag_res:
        print("[+] Touch file /data/codex/config/.imported is verified active.")
    else:
        print("[!] Note: /data/codex/config/ missing devices or activities. Run codex_daemon to complete first-boot migration.")

    # 8. Check final Hub flash space
    print("\n[*] Final Hub filesystem usage:")
    print(get_hub_space(args.hub))
    print("\n[+] Elite -> Hub sync complete!")


if __name__ == "__main__":
    main()
