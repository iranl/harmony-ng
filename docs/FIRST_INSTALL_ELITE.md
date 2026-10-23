# Harmony Elite 950 Remote - First Time Installation & Rooting Guide

This guide covers initial USB rooting and installation of the **HarmonyNG** custom firmware (`codex_elite`) on the Logitech Harmony Elite 950 handheld remote (Models N-R0007 / N-R0010).

> [!NOTE]
> **Harmony Ultimate (Home) & Touch Compatibility**:
> Harmony Ultimate (Home) and Harmony Touch share the same underlying hardware platform and USB interface. They **SHOULD** also be supported using the same tooling as Elite, but are currently untested, try at your own risk.

---

## 1. Prerequisites & Getting the Files

### Hardware & Driver Requirements
- **Remote**: Logitech Harmony Elite 950 (Model N-R0007 / N-R0010), or Harmony Ultimate (Home) / Touch (*untested, try at own risk*).
- **Micro-USB Cable**: Connect directly to the remote's bottom port (NOT the charging cradle base).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **RNDIS Network Driver**: Installed on host PC (see [`RNDIS_AND_NETWORK_SETUP.md`](RNDIS_AND_NETWORK_SETUP.md)).
- **Python 3**: Installed on host PC (`pip install pyusb`).

### Choose Your Installation Method

#### Option A: Release Bundle (Recommended — No Compilation)
1. Download `harmony-elite-v*.zip` (Windows) or `harmony-elite-v*.tar.gz` (Linux/macOS) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases).
2. Extract the archive into a folder on your PC.
3. Open a terminal in the extracted folder. All required binaries (`codex_elite`, `dropbearmulti_musl`, `b64dec_tiny`), scripts, and deployment tools are pre-packaged and ready to run.

#### Option B: From Git Repository (Developers)
```bash
git clone https://github.com/iranl/harmony-ng.git
cd harmony-ng

# Compile ARM userspace binaries
./build/compile_all.sh elite
```

---

## 2. Automated USB HID Rooting & Firmware Update

> [!IMPORTANT]
> **Up-to-Date Firmware Required**: HarmonyNG requires the latest official Logitech firmware (**v4.15.330** for Elite / 950). You do **not** need the proprietary Logitech desktop app or MyHarmony software. Our automated tooling checks, downloads, and flashes firmware directly over USB HID.

1. Connect the remote directly to your host PC via Micro-USB (bottom port).
2. Run the automated all-in-one setup tool:
   ```bash
   python elite/setup_remote.py
   ```
   *(Or `python tools/root_remote_usb.py`)*

The tool automatically:
1. Connects to the remote over USB HID (`046D:C12B`).
2. **Checks Firmware Version**: Queries `/sys/sysinfo` and compares with Logitech update servers. If the remote is not on latest v4.15.330, it prompts:
   ```text
   [!] Elite Remote firmware is 4.15.260, but latest official firmware is 4.15.330.
   [?] Download and install latest official firmware (4.15.330) first? (y/N):
   ```
   If confirmed, it downloads `elite_99_4.15.330.hfw2`, flashes it cleanly via USB HID, waits for the reboot, and resumes rooting automatically. *(Pass `--skip-fw-check` to bypass).*
3. Stages the self-extracting ARM Dropbear payload to `/sys/dropbear`.
4. Arms persistent boot trigger `/tde/enable` -> `/etc/tdeenable`.
5. Sends reboot signal to `/sys/reboot`.
6. Remote boots with Dropbear SSH listening on `192.168.2.1:22`.

> [!TIP]
> You can also check, download, or flash Elite firmware and extract UI icons at any time with the standalone tool:
> ```bash
> # Check or flash firmware directly:
> python tools/harmony_firmware.py check elite
> python tools/harmony_firmware.py flash elite
>
> # Extract all 1,350 official icons directly from firmware bundle (no remote connection needed):
> python tools/harmony_firmware.py extract-icons --archive build/icons.tar.gz
> ```

---

## 3. Create Stock Remote Backup (Recommended)

Before modifying flash contents, save a complete backup of stock Lua scripts and configuration:

```bash
# On host PC (plain uncompressed tar, no gzip):
mkdir -p backups/elite
ssh root@192.168.2.1 "tar -cf - /data /opt/luadraws /etc 2>/dev/null" > backups/elite/golden_elite_backup.tar
```

> [!NOTE]
> Backups and restores use uncompressed tar archives (`.tar`, not `.tar.gz`). Embedded BusyBox on the device does not use gzip to avoid memory and CPU bottlenecks on flash.

---

## 4. Deploy HarmonyNG Firmware

Run the deployment script to push `codex_elite` (touch UI & framebuffer engine) to the remote:

### On Windows (PowerShell):
```powershell
# From repo root:
.\elite\deploy_elite.ps1 -Ip 192.168.2.1

# Or from extracted release zip:
.\deploy_elite.ps1 -Ip 192.168.2.1
```

### On Linux / macOS / Windows (Python):
```bash
# From repo root:
python elite/deploy_elite.py --ip 192.168.2.1

# Or from extracted release zip:
python deploy_elite.py --ip 192.168.2.1
```

The script:
1. Copies `codex_elite` to `/data/codex_elite`.
2. Installs safe startup persistence into `/etc/init.d/rcS.local`.
3. Disables the stock `luadraws` runtime to free `/dev/fb0` and `/dev/rfspi`.
4. Starts `codex_elite` with immediate vector graphics rendering.

---

## 5. RF Pairing with Harmony Hub

1. On the Elite remote touch screen, tap the **Settings** dock icon.
2. Select **RF Pairing Menu** -> **Start Pairing to Hub**.
3. In the Hub Web console (`http://<YOUR_HUB_IP>:8089`), navigate to **Harmony Elite Remote** and click **Start Hub Pairing Mode**.
4. Within 5 seconds, the Hub and remote exchange RF security tokens, the status badge turns green, and activities and button maps synchronize automatically over 2.4 GHz RF.

---

## 6. Battery Life & Power Optimization Settings

The custom `codex_elite` firmware includes power management settings to maximize battery life:

1. **Button Backlight Control**:
   - In **Settings** on the touch screen, tap **Button Backlight**:
     - `OFF`: Always keeps physical keypad LEDs off (maximum battery savings).
     - `ON`: Illuminates keypads on remote pickup or button press.
     - `NIGHT`: Automatically turns off button backlights between **22:00 and 07:00** while keeping the screen active on touch.
2. **Gyroscope Poll Rate**:
   - Tap **Gyro Poll Rate** in Settings:
     - `10 Hz` (**Recommended**): Cuts pickup standby power by over 60% while detecting lift gestures.
     - `25 Hz`: Balanced response.
     - `50 Hz`: High sensitivity.

---

## 7. Factory Reset & Reverting to Stock Firmware

To completely remove HarmonyNG software and revert the Elite remote to official Logitech stock firmware:

```bash
# Using Python:
python tools/factory_reset.py --target elite --elite-ip 192.168.2.1

# Or using deployment script:
python elite/deploy_elite.py --ip 192.168.2.1 --factory-reset

# Or using PowerShell:
.\elite\deploy_elite.ps1 -Ip 192.168.2.1 -FactoryReset
```

---

## Appendix: Flash Storage Architecture & Memory Footprint

> [!NOTE]
> Provided for reference. The Elite remote features generous NAND capacity and flash wear is not an issue.

The Harmony Elite features a **128 MB NAND Flash** with 64 MB mobile DDR RAM:

| Partition | Mount Point | Filesystem | Total Size | Available Space | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `nand0,0` | `/` (root) | SquashFS | 16.0 MB | 0 KB (Read-Only) | Kernel 2.6.31-arm and base rootfs. |
| `nand0,1` | `/data` | JFFS2/UBIFS | 64.0 MB | **~32 MB Free** | Generous writable partition. |
| `nand0,2` | `/opt` | SquashFS/JFFS2 | 32.0 MB | ~8 MB Free | Stock Lua UI (`luadraws`) and HBUS binaries. |

- `codex_elite` binary: **~718 KB**
- Total HarmonyNG footprint on `/data`: **< 3 MB** out of 32 MB free space.
