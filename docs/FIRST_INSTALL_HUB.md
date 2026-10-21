# Harmony Hub - First Time Installation & Setup Guide

This guide walks you through rooting and installing **HarmonyNG** (`codex_daemon`) on a stock Logitech Harmony Hub (Model O-R0004).

---

## 1. Prerequisites & Getting the Files

### Hardware & Network Requirements
- **Harmony Hub** (Model O-R0004).
- **Micro-USB Cable**: Must support high-speed data transfer (not charge-only).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **RNDIS Network Driver**: Installed on host PC (see [`RNDIS_AND_NETWORK_SETUP.md`](RNDIS_AND_NETWORK_SETUP.md)).
- **Python 3**: Installed on host PC with `pip install pyusb`.

### Choose Your Installation Method

#### Option A: Release Bundle (Recommended — No Compilation)
1. Download the latest `harmony-hub-v*.zip` (Windows) or `harmony-hub-v*.tar.gz` (Linux/macOS) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases).
2. Extract the archive into a folder on your computer.
3. Open a terminal (PowerShell or Bash) in the extracted directory. All required binaries, scripts, modules, and deployment tools are pre-packaged and ready to run.

#### Option B: From Git Repository (Developers)
```bash
git clone https://github.com/iranl/harmony-ng.git
cd harmony-ng

# 1. Compile MIPS userspace binaries
./build/compile_all.sh hub

# 2. Compile WebUI bundle
cd android-app && npm install && npm run build && cd ..
```

---

## 2. Step-by-Step USB Rooting Procedure

If your Hub is running stock firmware with SSH disabled, use this one-time USB unlock procedure.

### Step 2.1: Put Hub into USB Diagnostic Mode
1. Unplug the Harmony Hub from power and USB.
2. Press and **hold the Pair / Reset button** on the back of the Hub.
3. While continuing to hold the button, connect the Micro-USB cable to your PC.
4. Keep holding the button for **15 to 20 seconds** until the front LED begins flashing rapidly (amber / fast red).
5. Release the button. The Hub is now in USB Recovery / Engineering mode.

### Step 2.2: Configure Host PC Static IP
Configure your host PC's RNDIS USB network adapter with a static IP:
- **IP Address**: `192.168.1.2`
- **Subnet Mask**: `255.255.255.0`
- **Default Gateway**: `192.168.1.1`

Test connectivity:
```bash
ping 192.168.1.1
```

### Step 2.3: Unlock SSH & Inject Root Key
Run the automated USB root unlock tool:
```bash
python tools/root_hub_usb.py
```
The tool enables Dropbear SSH on the Hub and injects your host SSH public key.

Verify root SSH access:
```bash
ssh root@192.168.1.1
# Output should show: Linux HarmonyHub 2.6.31.14 ...
exit
```

---

## 3. Create Stock Backup (Recommended)

Before modifying flash contents, create a safety archive of your original stock certificates, Wi-Fi configs, and factory state:

```bash
# Windows (PowerShell):
bash build/backup_hub.sh 192.168.1.1

# Linux / macOS:
./build/backup_hub.sh 192.168.1.1
```
*(If working inside an extracted release zip, run `backup_hub.sh 192.168.1.1` directly).*

The backup archive is saved to `backups/golden_hub_backup.tar.gz`.

---

## 4. Deploy HarmonyNG Firmware

Run the deployment script to push `codex_daemon`, the WebUI, network manager, and kernel modules to the Hub:

### On Windows (PowerShell):
```powershell
# From repo root:
.\build\deploy_hub.ps1 -HubIp 192.168.1.1

# Or from extracted release zip:
.\deploy_hub.ps1 -HubIp 192.168.1.1
```

### On Linux / macOS (Bash):
```bash
# From repo root:
./build/deploy_hub.sh 192.168.1.1

# Or from extracted release zip:
./deploy_hub.sh 192.168.1.1
```

The installer automatically:
1. Validates available flash memory before writing.
2. Installs `codex_daemon` (Hub micro-daemon) to `/data/codex/bin/`.
3. Deploys startup scripts and USB Ethernet kernel drivers.
4. Deploys the lightweight Web UI to `/data/codex/www/`.
5. Launches all services and verifies `http://192.168.1.1:8089/api/status`.

---

## 5. Accessing the Web Console & Android App

1. Connect the Hub to your home Wi-Fi or local network.
2. In your browser or companion app, navigate to:
   ```text
   http://<YOUR_HUB_IP>:8089
   ```
3. Use the header language selector for English, Deutsch, Français, Nederlands, or Español.
4. Download the Android companion app (`harmony-hub-release.apk`) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases) to control devices and activities directly from your phone.

---

## Appendix: Flash Storage Architecture & Safety Limits

> [!NOTE]
> The deployment scripts manage partition limits automatically. This section is provided for reference and advanced development.

The Harmony Hub has a single 16 MB SPI NOR flash shared between the Atheros AR9344 CPU and the CC2544 RF chip:

| Partition | Mount Point | Filesystem | Total Size | Available Space | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `mtd3` | `/` (root) | SquashFS | 4.0 MB | 0 KB (Read-Only) | Contains stock Dropbear (`/usr/sbin/dropbear`) and base utilities. |
| `mtd4` | `/mnt/data` | JFFS2 | 5.0 MB | **~1.25 MB Free** | **Configuration only!** Exceeding this partition corrupts JFFS2. |
| `mtd5` | `/cache` | JFFS2 | 5.0 MB | **~4.80 MB Free** | **Safe for binaries & modules.** Extended BusyBox and modules live here. |
| `tmpfs` | `/tmp` | RAM tmpfs | ~35 MB | ~34 MB Free | Runtime sockets, active logs, and PID files. |

- **Dropbear**: Takes **0 bytes** of writable flash because it is already embedded in the stock read-only SquashFS (`/usr/sbin/dropbear`).
- **Codex Micro-Daemon**: Lean C daemon (~370 KB), fits safely in `/data/codex/bin/`.
- **BusyBox (Enhanced)**: Pre-compiled standalone BusyBox (~1.6 MB) is installed to `/cache/bin/busybox`, never `/data/`.
