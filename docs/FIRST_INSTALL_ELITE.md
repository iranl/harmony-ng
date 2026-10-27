# Harmony Elite 950 & Pro 2400 Remote - First Time Installation & Rooting Guide

This guide covers initial USB rooting and installation of the **HarmonyNG** custom firmware (`elite_ui`) on the Logitech Harmony Elite 950 handheld remote (Models N-R0007 / N-R0010) and Harmony Pro 2400 Remote (Skin 116).

> [!NOTE]
> **Harmony Pro 2400 Remote, Ultimate (Home) & Touch Compatibility**:
> Reverse engineering confirms the **Harmony Pro 2400 Remote** (Skin 116, firmware 10.0.215) belongs to the exact same hardware family (`hops`) as the Elite 950, sharing the same TI DaVinci ARM SoC, 240x320 RGB565 LCD (`/dev/fb0`), Synaptics ClearPad touch digitizer, keypad matrix, and CC2544 RF transceiver (Report `0x20`).
>
> On firmware 10.0.215, Dropbear SSH is **already enabled by default** in `/etc/init.d/rcS`. The Pro 2400 Remote, Harmony Ultimate (Home), and Harmony Touch are all supported by our tooling and custom firmware (untested on physical Pro 2400 hardware, try at own risk).

---

## 1. Prerequisites & Getting the Files

### Hardware & Driver Requirements
- **Remote**: Logitech Harmony Elite 950 (Model N-R0007 / N-R0010), Harmony Pro 2400 Remote (Skin 116), or Harmony Ultimate (Home) / Touch (*untested, try at own risk*).
- **Micro-USB Cable**: Connect directly to the remote's bottom port (NOT the charging cradle base).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **USB network adapter with a static IP**: Windows needs the RNDIS driver; macOS and Linux need no driver. All need a static IP on the remote's subnet (see [`RNDIS_AND_NETWORK_SETUP.md`](RNDIS_AND_NETWORK_SETUP.md)).
- **Python 3**: Installed on host PC (`pip install pyusb`).

### Choose Your Installation Method

#### Option A: Release Bundle (Recommended — No Compilation)
1. Download `harmony-elite-v*.zip` (Windows) or `harmony-elite-v*.tar.gz` (Linux/macOS) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases).
2. Extract the archive into a folder on your PC.
3. Open a terminal in the extracted folder. All required binaries (`elite_ui`, `dropbearmulti_musl`, `b64dec_tiny`), scripts, and deployment tools are pre-packaged and ready to run.

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
   python tools/setup_remote.py
   ```

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
> python tools/harmony_firmware.py check
> python tools/harmony_firmware.py flash --target elite
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

Run the deployment script to push `elite_ui` (touch UI & framebuffer engine) to the remote:

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
1. Copies `elite_ui` to `/data/elite_ui`.
2. Installs safe startup persistence into `/etc/init.d/rcS.local`.
3. Disables the stock `luadraws` runtime to free `/dev/fb0` and `/dev/rfspi`.
4. Starts `elite_ui` with immediate vector graphics rendering.

---

## 5. RF Pairing with Harmony Hub

1. On the Elite remote touch screen, tap the **Settings** dock icon.
2. Select **RF Pairing Menu** -> **Start Pairing to Hub**.
3. In the Hub Web console (`http://<YOUR_HUB_IP>:8089`), navigate to **Harmony Elite Remote** and click **Start Hub Pairing Mode**.
4. Within 5 seconds, the Hub and remote exchange RF security tokens, the status badge turns green, and activities and button maps synchronize automatically over 2.4 GHz RF.

---

## 6. Battery Life & Power Optimization Settings

The custom `elite_ui` firmware includes power management settings to maximize battery life:

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

### Hardware Button Combination (Zero-SSH / Independent of Software)

To reset the remote to factory state without SSH or even if userland software is unresponsive:

1. **Press and hold simultaneously**:
   - The **`Menu`** button (physical key directly above the `CH+` button).
   - The **`Off`** button (power button at the top of the remote).
2. **Hold continuously for ~20 seconds**:
   - The screen will turn off, then reboot.
   - > [!IMPORTANT]
     > The graphic showing a remote with a circular arrow **flashes briefly first** before staying persistently on the screen. **You must keep holding both `Menu` and `Off` until the graphic stays permanently on the screen.** Do not release on the first flash.
3. **Release both buttons** once the circular arrow graphic remains steadily on the display.
4. The remote will wipe all user configurations and restore factory stock defaults.

### Software Stock Revert (Over SSH / USB Network)

To cleanly remove HarmonyNG software and restore stock Logitech binaries over SSH:

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

- `elite_ui` binary: **~718 KB**
- Total HarmonyNG footprint on `/data`: **< 3 MB** out of 32 MB free space.
