# Harmony Hub - First Time Installation & Setup Guide

This guide walks you through rooting and installing **HarmonyNG** (`codex_daemon`) on a stock Logitech Harmony Hub (Model O-R0004).

---

## 1. Prerequisites & Getting the Files

### Hardware & Network Requirements
- **Harmony Hub** (Model O-R0004).
- **Micro-USB Cable**: Data-capable cable (not charge-only).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **Python 3**: Installed on host PC (no external libraries needed; uses standard library).
- **Local Wi-Fi Network**: 2.4 GHz 802.11b/g/n (Harmony Hub connects via Wi-Fi).

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

## 2. Automated USB HID Rooting Procedure

If your Hub is running stock firmware with SSH disabled, use this one-time automated USB HID rooting procedure.

### Step 2.1: Connect Hub via Micro-USB
1. Connect the Harmony Hub directly to your host PC using a standard Micro-USB data cable.
2. The Hub powers on and enumerates on USB as `046D:C129` (Logitech Harmony Hub).

### Step 2.2: Run Automated USB Root Tool
Run the all-in-one root tool:

```bash
# If your Hub is already connected to your Wi-Fi network:
python tools/root_hub_usb.py --hub-ip <YOUR_HUB_IP>

# If your Hub is factory-reset or needs Wi-Fi credentials configured:
python tools/root_hub_usb.py --wifi-ssid "YourNetworkName" --wifi-pass "YourPassword"
```

The tool automatically:
1. Detects the Harmony Hub on USB HID (`046D:C129`).
2. Configures Wi-Fi credentials over USB HID if `--wifi-ssid` is supplied.
3. Stages the self-extracting MIPS Dropbear payload to `/sys/dropbear`.
4. Arms the persistent boot trigger `/tde/enable` -> `/etc/tdeenable`.
5. Sends reboot signal to `/sys/reboot`.
6. Hub restarts with Dropbear SSH permanently enabled on port 22 on your local network.

### Step 2.3: Verify SSH Access
Test SSH connectivity over your local network:
```bash
ssh -i ~/.ssh/harmony_owner_ed25519 root@<YOUR_HUB_IP>
# Output: Linux HarmonyHub 2.6.31.14 ...
exit
```

---

## 3. Create Stock Backup (Optional)

Before modifying flash contents, create a safety archive of your original stock certificates, Wi-Fi configs, and factory state:

```bash
# Windows (PowerShell):
bash build/backup_hub.sh <YOUR_HUB_IP>

# Linux / macOS:
./build/backup_hub.sh <YOUR_HUB_IP>
```
*(If working inside an extracted release zip, run `backup_hub.sh <YOUR_HUB_IP>` directly).*

The backup archive is saved to `backups/golden_hub_backup.tar.gz`.

---

## 4. Deploy HarmonyNG Firmware

Run the deployment script to push `codex_daemon`, the WebUI, network manager, and kernel modules to the Hub:

### On Windows (PowerShell):
```powershell
# From repo root:
.\build\deploy_hub.ps1 -HubIp <YOUR_HUB_IP>

# Or from extracted release zip:
.\deploy_hub.ps1 -HubIp <YOUR_HUB_IP>
```

### On Linux / macOS (Bash):
```bash
# From repo root:
./build/deploy_hub.sh <YOUR_HUB_IP>

# Or from extracted release zip:
./deploy_hub.sh <YOUR_HUB_IP>
```

The installer automatically:
1. Validates available flash memory before writing.
2. Installs `codex_daemon` (Hub micro-daemon) to `/data/codex/bin/`.
3. Deploys startup scripts and USB Ethernet kernel drivers.
4. Deploys the lightweight Web UI to `/data/codex/www/`.
5. Launches all services and verifies `http://<YOUR_HUB_IP>:8089/api/status`.

---

## 5. Accessing the Web Console & Android App

1. In your browser or companion app, navigate to:
   ```text
   http://<YOUR_HUB_IP>:8089
   ```
2. Use the header language selector for English, Deutsch, Français, Nederlands, or Español.
3. Download the Android companion app (`harmony-hub-release.apk`) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases) to control devices and activities directly from your phone.

---