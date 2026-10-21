# Harmony Hub - First Time Installation & Setup Guide

This guide covers the initial installation of the **Harmony Hub Control** custom firmware (`codex_daemon`) on a stock Logitech Harmony Hub.

---

## 1. Prerequisites

- **Hardware**: Logitech Harmony Hub (Model O-R0004).
- **Cable**: High-quality Micro-USB cable capable of high-speed data transfer and power (not charge-only).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **RNDIS Network Driver**: Installed on host PC (see [`RNDIS_AND_NETWORK_SETUP.md`](RNDIS_AND_NETWORK_SETUP.md)).
- **SSH Client**: OpenSSH (`ssh` in PowerShell/Terminal) or PuTTY.

---

## 2. Flash Storage Architecture & Safety Limits

> [!CAUTION]
> **FLASH CAPACITY WARNING**: The Harmony Hub has a single 16 MB SPI NOR flash shared between the CPU and the RF chip.
> Do NOT copy large binaries or packages into `/mnt/data` (`/data`)!

| Partition | Mount Point | Filesystem | Total Size | Typical Free Space | Safe for Deployment? |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `mtd3` | `/` (root) | SquashFS | 4.0 MB | 0 KB (Read-Only) | Contains stock Dropbear (`/usr/sbin/dropbear`) & BusyBox 1.13.4. |
| `mtd4` | `/mnt/data` | JFFS2 | 5.0 MB | **~1,250 KB** | **Configuration only!** Exceeding this partition corrupts JFFS2 and bricks the Hub. |
| `mtd5` | `/cache` | JFFS2 | 5.0 MB | **~4,800 KB** | **Safe for binaries.** Store custom BusyBox or tools in `/cache/bin/`. |
| `tmpfs` | `/tmp` | RAM tmpfs | ~35 MB | ~34 MB | Safe for logs, volatile PID files, and runtime sockets. |

- **Dropbear**: Takes **0 bytes** of writable flash because it is already embedded in the stock read-only SquashFS (`/usr/sbin/dropbear`).
- **Codex Micro-Daemon**: Only ~610 KB, safely fits in `/data/codex/bin/` or `/cache/bin/`.
- **BusyBox (Enhanced)**: Pre-compiled standalone BusyBox is ~1.6 MB. **Must be installed to `/cache/bin/busybox`**, NEVER `/data/`!

---

## 3. Step-by-Step USB Rooting Procedure

If your Hub is running stock firmware with SSH disabled, use this USB unlock procedure.

### Step 3.1: Trigger USB Recovery / Diagnostic Mode
1. Unplug the Harmony Hub from power and USB.
2. Press and **hold the Pair / Reset button** on the back of the Hub.
3. While continuing to hold the button, connect the Micro-USB cable to your host PC.
4. Keep holding the button for **15 to 20 seconds** until the front LED begins flashing rapidly (amber / fast red).
5. Release the button. The Hub is now in USB Recovery / Engineering mode.

### Step 3.2: Verify USB Interface & Configure Host IP
1. Open Device Manager (Windows) or run `dmesg` / `lsusb` (Linux/macOS):
   - You should see a USB Ethernet / RNDIS Gadget device (VID `046d`, PID `c11f` or `c124`).
2. Configure your host PC's RNDIS network adapter with static IP:
   - **IP Address**: `192.168.1.2`
   - **Subnet Mask**: `255.255.255.0`
   - **Default Gateway**: `192.168.1.1`
3. Test connectivity:
   ```bash
   ping 192.168.1.1
   ```

### Step 3.3: Unlock SSH & Inject Authorized Key
In diagnostic / recovery mode, the internal dropbear service can be started directly via the local management port or diagnostic endpoint:

1. Send the unlock trigger via `curl`:
   ```bash
   # Query diagnostic endpoint to trigger dropbear daemon
   curl -s -X POST http://192.168.1.1:8080/api/v1/diagnostics/ssh -d '{"enable": true}' 2>/dev/null || true
   ```
2. Or use the automated USB root tool:
   ```bash
   python tools/root_hub_usb.py
   ```
3. Copy your SSH public key to the Hub's persistent authorized keys:
   ```bash
   ssh -o StrictHostKeyChecking=no root@192.168.1.1 "mkdir -p /mnt/data/etc/dropbear"
   scp ~/.ssh/id_rsa.pub root@192.168.1.1:/mnt/data/etc/dropbear/authorized_keys
   ssh root@192.168.1.1 "chmod 600 /mnt/data/etc/dropbear/authorized_keys"
   ```
4. Verify root access:
   ```bash
   ssh root@192.168.1.1
   uname -a
   # Linux HarmonyHub 2.6.31.14 #1 PREEMPT mips GNU/Linux
   ```

---

## 4. Create Golden First Backup (Mandatory!)

**Before modifying any files on flash**, run the automated backup script to create an untouched archive of your stock certificates and resources:

```bash
# Run from repository root:
./build/backup_hub.sh 192.168.1.1
```

The script saves:
- `backups/golden_hub_backup.tar.gz`: Untouched stock `/data/resources/`, network configs, and certificates.
- `backups/hub_backup_<timestamp>.tar.gz`: Rolling backup archive.

---

## 5. Deploy Custom Codex Firmware

Run the deployment script:
```bash
# Standard user deployment (uses stock BusyBox; preserves flash space):
./build/deploy_hub.sh 192.168.1.1
# Or PowerShell:
.\deploy_hub.ps1 -HubIp 192.168.1.1

# Developer / Debug mode (installs extended BusyBox to /cache/bin/):
./build/deploy_hub.sh 192.168.1.1 --debug
# Or PowerShell:
.\deploy_hub.ps1 -HubIp 192.168.1.1 -DebugBusybox
```

The script performs the following safe actions:
1. Verifies available flash on `/mnt/data` and `/cache`.
2. Cleanly terminates stock and old processes (`killall -9 codex_daemon codex_rf codex_btstack 2>/dev/null`).
3. Installs `codex_daemon` (610 KB) to `/data/codex/bin/`.
4. Installs `codex_rf` (CC2544 engine) and `codex_btstack` (Bluetooth engine).
5. Deploys the pre-compressed WebUI (`index.html.gz` ~23 KB).
6. Installs extended BusyBox to `/cache/bin/busybox` **only** if `--debug` / `-DebugBusybox` is set. Subsequent regular deployments will never touch or remove an existing BusyBox installation.
7. Launches services and validates `http://192.168.1.1:8089/api/status`.

---

## 6. Accessing the Web Console & Mobile App

1. Connect the Hub to your home Wi-Fi or local network.
2. In your browser or Android app, open:
   ```text
   http://<YOUR_HUB_IP>:8089
   ```
3. Use the language selector in the top-right header to switch between **English**, **Deutsch**, **Français**, **Nederlands**, or **Español**.
4. Test API health:
   ```bash
   python tests/test_live_api.py --host <YOUR_HUB_IP> --port 8089
   ```
