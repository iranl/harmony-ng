# Harmony Hub - First Time Installation & Setup Guide

This guide walks you through rooting and installing **HarmonyNG** (`hub_daemon`) on a stock Logitech Harmony Hub (Standard Model O-R0004 or Harmony Pro 2400).

---

## 1. Prerequisites & Getting the Files

### Hardware & Network Requirements
- **Harmony Hub**: Standard Hub (Model O-R0004, USB `046D:C129`) or Harmony Pro 2400 (PoE/RJ45, USB `046D:C132`).
- **Micro-USB Cable**: Data-capable cable (not charge-only).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **Python 3**: Installed on host PC (no external libraries needed; uses standard library).
- **Local Network**: 2.4 GHz 802.11b/g/n Wi-Fi, or direct RJ45 Ethernet cable (Pro 2400).

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

> [!NOTE]
> The automatic stock backup in `tools/root_hub_usb.py` needs a MIPS BusyBox with `tar` (the stock one has none). The release bundle ships it as `bin/busybox`; a git checkout does not, so build it first with `./build/build_busybox.sh mips` (output: `payload/bin/busybox`), otherwise the root tool cannot take the backup.

---

## 2. Automated USB HID Rooting & Firmware Update

> [!IMPORTANT]
> **Up-to-Date Firmware Required**: HarmonyNG requires the latest official Logitech firmware (**v4.15.600** for standard Hub, or **v10.0.230** for Harmony Pro 2400). You do **not** need the proprietary Logitech desktop app or MyHarmony software. Our automated tooling checks, downloads, and flashes firmware directly over USB HID.

If your Hub is running stock firmware with SSH disabled, use this one-time automated USB HID rooting procedure.

### Step 2.1: Connect Hub via Micro-USB
1. Connect the Harmony Hub directly to your host PC using a standard Micro-USB data cable.
2. The Hub powers on and enumerates on USB as `046D:C129` (Standard Hub) or `046D:C132` (Harmony Pro 2400).

### Step 2.2: Run Automated USB Root Tool
Run the all-in-one root tool:

```bash
# If your Hub is already connected to your local network (Wi-Fi or Pro 2400 WiFi/Ethernet):
python tools/root_hub_usb.py --hub-ip <YOUR_HUB_IP>

# If your Hub is factory-reset or needs Wi-Fi credentials configured:
python tools/root_hub_usb.py --wifi-ssid "YourNetworkName" --wifi-pass "YourPassword"
```

The tool automatically:
1. Detects the Harmony Hub on USB HID (`046D:C129` or `046D:C132`).
2. **Checks Firmware Version**: Queries `/sys/sysinfo` over USB HID and checks against the official Logitech SUS update servers (v4.15.600 for standard Hub Skin 97, v10.0.230 for Pro 2400 Skin 115). If the Hub is not running latest firmware, it prompts:
   ```text
   [!] Hub firmware is 4.15.250, but latest official firmware is 4.15.600.
   [?] Download and install latest official firmware (4.15.600) first? (y/N):
   ```
   If confirmed, it downloads the official firmware, flashes it cleanly via USB HID, waits for the reboot, and resumes rooting automatically. *(Pass `--skip-fw-check` to bypass).*
3. Configures Wi-Fi credentials over USB HID if `--wifi-ssid` is supplied.
4. Stages the self-extracting MIPS Dropbear payload to `/sys/dropbear`.
5. Arms the persistent boot trigger `/tde/enable` -> `/etc/tdeenable`.
6. Sends reboot signal to `/sys/reboot`.
7. Hub restarts with Dropbear SSH permanently enabled on port 22 on your local network.
8. **Automatically backs up stock partitions**: Streams the entire `/cache` and `/data` partitions to `backups/stock_hub_backup_<timestamp>.tar` and `backups/stock_hub_backup_latest.tar` (uncompressed tar; no gzip is used to avoid memory/CPU exhaustion and BusyBox compression incompatibilities on embedded MIPS).

> [!TIP]
> You can also check, download, or flash Hub firmware at any time with the standalone tool:
> ```bash
> python tools/harmony_firmware.py check
> python tools/harmony_firmware.py flash --target hub
> ```

### Step 2.3: Verify SSH Access
Test SSH connectivity over your local network:
```bash
ssh -i ~/.ssh/harmony_owner_ed25519 root@<YOUR_HUB_IP>
# Output: Linux HarmonyHub 2.6.31.14 ...
exit
```

---

## 3. Stock Backup & Recovery Safety

When rooting via `tools/root_hub_usb.py`, a complete backup of your Hub's stock state (entire `/cache` and `/data` partitions) is automatically saved to:
```text
backups/stock_hub_backup_<timestamp>.tar
backups/stock_hub_backup_latest.tar
```

> [!NOTE]
> **No gzip compression**: Backups and restores use uncompressed tar archives (`.tar`, not `.tar.gz`). The embedded BusyBox on the Hub has limited RAM and flash bandwidth; omitting gzip avoids CPU exhaustion and out-of-memory kernel panics during tar extraction.

You can also run manual backups at any time:
```bash
# Cross-platform (Windows, Linux, macOS):
python tools/backup_hub.py <YOUR_HUB_IP>
```

This archive preserves your stock Logitech pairings, Wi-Fi configuration, device profiles, and certificates. You can restore from it at any time using `python tools/restore_stock.py`.

---

## 4. Deploy HarmonyNG Firmware

Run the deployment script to push `hub_daemon`, the WebUI, network manager, and kernel modules to the Hub:

```bash
# Cross-platform (Windows, Linux, macOS):
# From repo root:
python build/deploy_hub.py <YOUR_HUB_IP>

# Or from extracted release zip:
python deploy_hub.py <YOUR_HUB_IP>
```

Add `--dry-run` (or `-n`) to see what a deployment would change without changing anything: it reads the Hub's checksums, lists the files it would upload or purge and the steps it would run (backup, service stop, restart or reboot), and leaves the Hub and the local tree as they are. It does not build the web UI; from the repository it compares the files already in `payload/www`.

The installer automatically:
1. Validates available flash memory before writing.
2. Installs `hub_daemon` (Hub micro-daemon) to `/data/harmony/bin/`.
3. Deploys startup scripts and USB Ethernet kernel drivers.
4. Deploys the lightweight Web UI to `/data/harmony/www/`.
5. **Auto-extracts Remote UI Icons**: If `build/icons.tar.gz` is not yet built, the script asks:
   ```text
   [?] build/icons.tar.gz not found on host.
   Download official remote firmware, extract 1,350 icons, and build build/icons.tar.gz now? (Y/n)
   ```
   If confirmed, it downloads the official Elite firmware (`remote_111_4.15.330.hfw2`), extracts all 1,347 device & activity icons from the internal SquashFS filesystem, packages them into `build/icons.tar.gz`, and stages them to `/cache/icons.tar.gz` on the Hub.
6. Launches all services and verifies `http://<YOUR_HUB_IP>:8089/api/status`.

### Stock configuration import

On the first deploy (no `/data/harmony/config/devices.json` on the Hub yet), the deploy script copies the stock Logitech files from `/data/resources` and converts them with `tools/migrate_hub_configs.py`:

| Stock | HarmonyNG |
| :--- | :--- |
| Devices and their commands (`DeviceList.json`) | `devices.json`, with short device ids (`1`, `2`, ...) |
| Power settings (`PowerFeature`): power-on delay, discrete on/off actions | Each device's `powerOnDelay`, `powerOnSequence` and `powerOffSequence`. Toggle-only devices keep empty sequences and use their `PowerToggle` command |
| Activities, their devices and the input each device is set to (`ActivityList.json` roles) | `activities.json`, with the input commands at the start of each activity's start sequence |
| Remote button maps (`MapList.json`) | `remote_slot1_map.json`: the keys of each activity, the keys of each device (device browser) and each activity's screen buttons |

Not imported; the migration prints a warning for each:
- Long presses, double presses and touch gestures (HarmonyNG has no equivalent yet).
- Inputs that the stock config selects by stepping through a Source key (common on soundbars). Add the steps to the activity by hand.
- Commands of a second transport on the same device, e.g. the Bluetooth keyboard commands of an IR TV.
- Screen buttons beyond the config push limit of 240 per remote, which is shared out between the activities.

**Bluetooth devices** (e.g. an NVIDIA Shield) have to be paired again: HarmonyNG uses its own Bluetooth stack, so the stock pairings are not carried over.

The deploy only imports when the Hub has no configuration yet. To run the import again, for example to read its warnings, on a machine with this repository:

```bash
mkdir -p stock
scp -O -r root@<YOUR_HUB_IP>:/data/resources stock/
python3 tools/migrate_hub_configs.py --src stock/resources --dest migrated
```

To use the result, back up `/data/harmony` on the Hub first, copy `migrated/config/*` to `/data/harmony/config/` and `migrated/*.json` to `/data/harmony/`, then reboot the Hub.

---

## 5. Accessing the Web Console & Android App

1. In your browser or companion app, navigate to:
   ```text
   http://<YOUR_HUB_IP>:8089
   ```
2. Use the header language selector for English, Deutsch, Français, Nederlands, or Español.
3. Download the Android companion app (`harmony-hub-release.apk`) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases) to control devices and activities directly from your phone.

---

## 6. Factory Reset & Returning to Stock

If you ever wish to revert any changes, test official firmware, or recover from an unexpected state:

### Hardware Factory Reset (Zero-Disassembly Method)
1. **Unplug the micro-USB cable** from the back of the Hub.
2. **Press and hold the Pair/Reset button** on the rear of the Hub.
3. While holding the button, **plug the micro-USB cable into your PC**.
4. Keep holding for **15–20 seconds** until the front LED flashes red rapidly, then release.
5. The Hub formats the writable overlay (`/data` and `/cache`) and reboots to pristine stock SquashFS firmware (~30–45s).
6. When complete, the front LED pulses red slowly.

### Restore Stock with Full Configuration
To restore all your original Wi-Fi settings, devices, and activities from your automatic backup:

```bash
# All-in-one restoration:
python tools/restore_stock.py

# Or specify a custom backup file:
python tools/restore_stock.py --backup backups/stock_hub_backup_latest.tar

# Or restore pure stock with SSH removed:
python tools/restore_stock.py --pure-stock
```

This automated tool gains root over USB HID, terminates background processes, extracts your backed-up `/cache` and `/data` partitions (plain uncompressed tar stream, no gzip on Hub), flushes SPI flash, and cleanly reboots back into your configured stock Logitech system.

---