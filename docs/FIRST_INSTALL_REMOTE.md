# Harmony Touchscreen Remote - First Time Installation & Rooting Guide

This guide covers initial USB rooting and installation of the **HarmonyNG** custom firmware (`remote_ui`) on the Logitech Harmony touchscreen remotes, such as the Harmony Elite 950 (Models N-R0007 / N-R0010), the Harmony Ultimate and the Harmony Pro 2400 Remote (Skin 116).

> [!NOTE]
> **Harmony Pro 2400 Remote, Ultimate (Home) & Touch Compatibility**:
> Reverse engineering confirms the **Harmony Pro 2400 Remote** (Skin 116, firmware 10.0.215) belongs to the exact same hardware family (`hops`) as the Elite 950, sharing the same TI DaVinci ARM SoC, 240x320 RGB565 LCD (`/dev/fb0`), Synaptics ClearPad touch digitizer, keypad matrix, and CC2544 RF transceiver (Report `0x20`).
>
> On firmware 10.0.215, Dropbear SSH is **already enabled by default** in `/etc/init.d/rcS`. The Pro 2400 Remote, Harmony Ultimate (Home), and Harmony Touch are all supported by our tooling and custom firmware (untested on physical Pro 2400 hardware, try at own risk).
>
> Tested end to end on a **Harmony Ultimate (2013, skin 100, firmware 4.15.330)**: rooting, stock backup (see the warning in section 3), `remote_ui`, touch and RF pairing. Its differences from the Elite are listed under [Harmony Ultimate (2013)](#harmony-ultimate-2013).

---

## 1. Prerequisites & Getting the Files

### Hardware & Driver Requirements
- **Remote**: Logitech Harmony Elite 950 (Model N-R0007 / N-R0010), Harmony Pro 2400 Remote (Skin 116), Harmony Ultimate (2013, skin 100, tested), or Harmony Ultimate Home / Touch (*untested, try at own risk*).
- **Micro-USB Cable**: Connect directly to the remote's bottom port (NOT the charging cradle base).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **USB network adapter with a static IP**: Windows needs the RNDIS driver; macOS and Linux need no driver. All need a static IP on the remote's subnet (see [`RNDIS_AND_NETWORK_SETUP.md`](RNDIS_AND_NETWORK_SETUP.md)).
- **Python 3**: Installed on host PC (`pip install pyusb`).

### Choose Your Installation Method

#### Option A: Release Bundle (Recommended — No Compilation)
1. Download `harmony-remote-v*.zip` (Windows) or `harmony-remote-v*.tar.gz` (Linux/macOS) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases).
2. Extract the archive into a folder on your PC.
3. Open a terminal in the extracted folder. All required binaries (`remote_ui`, `dropbearmulti_musl`, `b64dec_tiny`), scripts, and deployment tools are pre-packaged and ready to run.

#### Option B: From Git Repository (Developers)
```bash
git clone https://github.com/iranl/harmony-ng.git
cd harmony-ng

# Compile ARM userspace binaries
./build/compile_all.sh remote
# BusyBox with tar, used for the remote backup (the release bundle ships it in bin/)
./build/build_busybox.sh arm
```

---

## 2. Automated USB HID Rooting & Firmware Update

> [!IMPORTANT]
> **Up-to-Date Firmware Required**: HarmonyNG requires the latest official Logitech firmware (**v4.15.330** for Elite / Ultimate / 950). You do **not** need the proprietary Logitech desktop app or MyHarmony software. Our automated tooling checks, downloads, and flashes firmware directly over USB HID.

1. Connect the remote directly to your host PC via Micro-USB (bottom port).
2. Check USB connection and driver status at any time:
   ```bash
   python remote/setup_remote.py --check-usb
   ```
3. Root the remote without installing anything yet, so you can take the stock backup in section 3 first:
   ```bash
   python remote/setup_remote.py --step 1
   ```
   Running `python remote/setup_remote.py` without `--step` also works, but when the remote is already in USB Ethernet mode it goes straight on to step 2, which installs `remote_ui` and stops the stock UI before you have a backup.

> [!TIP]
> **Windows 10 & 11 Automated Setup**: If Windows binds the remote as a serial COM port or fails to bind RNDIS after reboot, `setup_remote.py` automatically prompts for Administrator elevation to install the included WHQL-signed RNDIS driver and configure `192.168.2.2/24` (no manual Device Manager or WSL2 needed). You can also run the standalone installer at any time:
> ```powershell
> powershell -ExecutionPolicy Bypass -File tools/setup_remote_driver.ps1
> ```

The tool automatically:
1. **Detects USB & Network State**: Checks whether the remote is connected in USB HID mode, USB Ethernet mode, missing Windows RNDIS driver (Code 28), or disconnected.
2. **Validates Windows Driver & Host Static IP**: Confirms the host network adapter has IP `192.168.2.2/24` assigned, giving exact PowerShell commands if unconfigured.
3. **Validates & Displays Firmware Version**: Queries `/sys/sysinfo` over USB HID and checks against official Logitech SUS servers. *(Failure to read firmware mode over USB HID safely halts execution).* If the remote is not on latest v4.15.330, it prompts:
   ```text
   [!] Remote firmware is 4.15.260, but latest official firmware is 4.15.330.
   [?] Download and install latest official firmware (4.15.330) first? (y/N):
   ```
   If confirmed, it downloads `remote_111_4.15.330.hfw2`, flashes it cleanly via USB HID, waits for the reboot, and resumes rooting automatically. *(Pass `--skip-fw-check` to bypass).*
4. **Dynamic Execution**: If already in USB Ethernet mode with HTTP/SSH active, it skips HID staging and proceeds directly to permanent installation. Otherwise, it stages the self-extracting Dropbear payload to `/sys/dropbear`, arms `/tde/enable`, and reboots into multi-user mode.
5. **Post-Step Status**: Prints verified connection state upon completion.


> [!TIP]
> You can also check, download, or flash remote firmware and extract UI icons at any time with the standalone tool:
> ```bash
> # Check or flash firmware directly:
> python tools/harmony_firmware.py check
> python tools/harmony_firmware.py flash --target remote
>
> # Extract all 1,350 official icons directly from firmware bundle (no remote connection needed):
> python tools/harmony_firmware.py extract-icons --archive build/icons.tar.gz
> ```

---

## 3. Stock Remote Backup (Automatic)

As soon as SSH is up, `setup_remote.py` saves `/data`, `/opt/luadraws` and `/etc` of the remote to `backups/remote/remote_backup_<timestamp>.tar`, before the HarmonyNG runtime is installed. The first backup is also kept as `backups/remote/golden_remote_backup.tar`, which later runs never overwrite.

The stock BusyBox on the Elite, 950, Touch and Ultimate has no `tar`. The tool copies the project's own ARM BusyBox to `/tmp/busybox` (RAM) for the backup and removes it afterwards; nothing is written to flash. It looks for that BusyBox in `bin/` of the release bundle or in `remote/bin/` after `./build/build_busybox.sh arm`.

To make another backup of a rooted remote at any time:

```bash
python remote/setup_remote.py --backup
```

Pass `--no-backup` to skip the automatic backup during setup.

> [!NOTE]
> Backups and restores use uncompressed tar archives (`.tar`, not `.tar.gz`). Embedded BusyBox on the device does not use gzip to avoid memory and CPU bottlenecks on flash.

---

## 4. Deploy HarmonyNG Firmware

Run the deployment script to push `remote_ui` (touch UI & framebuffer engine) to the remote:

```bash
# Cross-platform (Windows, Linux, macOS):
# From repo root:
python remote/deploy_remote.py --ip 192.168.2.1

# Or from extracted release zip:
python deploy_remote.py --ip 192.168.2.1
```

The script:
1. Checks that the remote is ready (its commands, a BusyBox with `tar` for the themes, and space on `/data`), and stops with what to do if not. `python remote/deploy_remote.py --check` runs only this check.
2. Copies `remote_ui` to `/data/remote_ui`.
3. Installs safe startup persistence into `/etc/init.d/rcS.local`.
4. Disables the stock `luadraws` runtime to free `/dev/fb0` and `/dev/rfspi`.
5. Starts `remote_ui` with immediate vector graphics rendering.

---

## 5. RF Pairing with Harmony Hub

1. On the remote's touch screen, tap the **Settings** dock icon.
2. Select **RF Pairing Menu** -> **Start Pairing to Hub**.
3. In the Hub Web console (`http://<YOUR_HUB_IP>:8089`), navigate to **Harmony RF Remote** and click **Start Hub Pairing Mode**.
4. Within 5 seconds, the Hub and remote exchange RF security tokens, the status badge turns green, and activities and button maps synchronize automatically over 2.4 GHz RF.

---

## 6. Battery Life & Power Optimization Settings

The custom `remote_ui` firmware includes power management settings to maximize battery life:

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
python tools/factory_reset.py --target remote --remote-ip 192.168.2.1

# Or using deployment script:
python remote/deploy_remote.py --ip 192.168.2.1 --factory-reset
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

- `remote_ui` binary: **~718 KB**
- Total HarmonyNG footprint on `/data`: **< 3 MB** out of 32 MB free space.

### Harmony Ultimate (2013)

Observed on a Harmony Ultimate (hostname `juniper`, skin 100, firmware 4.15.330, kernel 2.6.37, armv5tejl). There are no separate `/data` and `/opt` partitions: the root filesystem is a unionfs with a writable UBIFS layer over a read-only SquashFS image, plus a separate `/cache` UBIFS volume.

```text
rootfs / rootfs rw
ubi0:cache /cache ubifs rw
/dev/mtdblock6 /mnt/root squashfs ro
ubi0:data /mnt/data ubifs rw
none / unionfs rw,dirs=/mnt/data=rw:/mnt/root=ro
```

`/data`, `/opt` and `/etc` are directories in that union, and everything written to them lands in the `ubi0:data` layer. The hardware factory reset (Menu + Off) wipes that layer: after a reset `/data/resources` and `/etc/tdeenable` are gone and the remote boots the stock firmware again.

Other differences from the Elite 950:

- **Skin 100.** Logitech's update service has no firmware entry for it; `setup_remote.py` looks up the remote firmware under skin 111 (the 4.15.330 remote bundle lists skin 100 among its intended devices).
- **Touch controller.** The Synaptics ClearPad TM2176-003 reports `ABS_X` 0-782 and `ABS_Y` 0-1009 instead of screen pixels; `remote_ui` scales these to the 240x320 screen.
- **Capacitive keys.** The star and home touch keys send `KEY_FAVORITES` (364) and `KEY_HOME` (102), like the Elite's capacitive buttons, and open **ACTIVITIES** and **DEVICES**. One touch can register as 2 or 3 presses.
- **No `tar`, `find` or `dd`** in the stock BusyBox (the same as on a stock Elite); `setup_remote.py` stages the project's own BusyBox in RAM for its backup, see section 3.
