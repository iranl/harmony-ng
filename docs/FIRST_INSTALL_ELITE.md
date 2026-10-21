# Harmony Elite 950 Remote - First Time Installation & Rooting Guide

This guide covers initial USB rooting and installation of the **HarmonyNG** custom firmware (`codex_elite`) on the Logitech Harmony Elite 950 handheld remote (Models N-R0007 / N-R0010).

---

## 1. Prerequisites

- **Remote**: Logitech Harmony Elite 950 (Model N-R0007 / N-R0010).
- **Cable**: Direct Micro-USB cable connected directly to the remote's port (NOT the charging cradle base).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **RNDIS Network Driver**: Installed on host PC (see [`RNDIS_AND_NETWORK_SETUP.md`](RNDIS_AND_NETWORK_SETUP.md)).
- **SSH Client**: OpenSSH (`ssh` in PowerShell/Terminal) or PuTTY.

---

## 2. Flash Storage Architecture & Memory Footprint

Unlike the Hub's compact 16 MB NOR flash, the Harmony Elite features a **128 MB NAND Flash** paired with 64 MB mobile DDR RAM:

| Partition | Mount Point | Filesystem | Total Size | Available Space | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `nand0,0` | `/` (root) | SquashFS | 16.0 MB | 0 KB (Read-Only) | Contains kernel 2.6.31-arm and base userspace. |
| `nand0,1` | `/data` | JFFS2/UBIFS | 64.0 MB | **~32.0 MB Free** | Generous writable partition. |
| `nand0,2` | `/opt` | SquashFS/JFFS2 | 32.0 MB | ~8.0 MB Free | Stock Lua UI (`luadraws`) and HBUS binaries. |

### Flash Capacity Analysis
- `codex_elite` binary: **~718 KB**
- Enhanced `busybox` (ARM): **~2.1 MB**
- **Conclusion**: The entire custom payload consumes less than **3 MB** out of the 32 MB free on `/data`. Flash exhaustion is not an issue on the Elite remote.

---

## 3. Step-by-Step USB Rooting Options (Touchscreen NOT Required)

If your Elite remote is on stock firmware with SSH disabled, choose any of the three available methods. None of them require a functioning touchscreen.

### Method A: Hardware Recovery Mode (Recommended for Broken Screens)
*Works regardless of touchscreen condition or firmware state.*

1. Ensure the remote is charged to at least 50%.
2. Disconnect the remote from the USB cable and charging cradle.
3. Press and hold both the **Menu** and **Mute** buttons simultaneously for **15 seconds**.
   - *(Alternative combo for stubborn firmware: Hold the physical **Off** button while inserting the Micro-USB cable).*
4. The remote screen will flash and display the USB connection gear icon.
5. Release the buttons. The remote boots into the RNDIS USB gadget mode.
6. Configure static IP on your host PC:
   - **IP Address**: `192.168.2.2`
   - **Subnet Mask**: `255.255.255.0`
   - **Default Gateway**: `192.168.2.1`
7. Test network link:
   ```bash
   ping 192.168.2.1
   ```
8. Deploy custom runtime and persistence:
   ```bash
   python elite/setup_remote.py --step 2
   # Or directly:
   python elite/deploy_elite.py
   ```

### Method B: Physical Keypad Debug Menu (No Touchscreen Needed)
*Works on running stock remotes when screen displays but touch digitizer is broken.*

1. With the remote powered on and connected via Micro-USB to PC:
2. On the physical keypad, press simultaneously:
   **`>>` (Fast Forward) + `Stop` + `Red button`**
3. The developer/debug menu opens on the remote screen.
4. Use the physical **Direction Down / Direction Up** navigation pad to highlight **"Enable USB Ethernet"** (or "USB Ethernet: ON").
5. Press the physical **Center / OK** button to activate.
6. A new USB Ethernet adapter appears on your PC. Configure static IP `192.168.2.2/24`.
7. Run the automated installer:
   ```bash
   python elite/setup_remote.py
   ```

### Method C: Direct USB HID Staging (`root_remote_usb.py`)
*Fully automated script over USB HID (no buttons or touchscreen).*

1. Plug the remote via Micro-USB directly into PC.
2. Run the dedicated USB HID root tool:
   ```bash
   python tools/root_remote_usb.py
   ```
3. The tool writes the ARM Dropbear multi-call payload to `/sys/dropbear`, arms `/tde/enable`, and sends `/sys/reboot`.
4. Remote reboots into rooted mode with Dropbear SSH online on `192.168.2.1:22`.

---

## 4. Create Golden Remote Backup (Mandatory!)

Before modifying any stock files or stopping stock services, create a complete backup:

```bash
# On host PC:
mkdir -p backups/elite
ssh root@192.168.2.1 "tar -czf - /data /opt/luadraws /etc 2>/dev/null" > backups/elite/golden_elite_backup.tar.gz
```

---

## 5. Deploying HarmonyNG Firmware

You can deploy automatically using the deployment script:
```bash
# Standard user deployment (uses stock BusyBox):
python elite/deploy_elite.py --ip 192.168.2.1
# Or PowerShell:
.\elite\deploy_elite.ps1 -Ip 192.168.2.1

# Developer / Debug mode (installs extended BusyBox to /data/busybox):
python elite/deploy_elite.py --ip 192.168.2.1 --debug-busybox
# Or PowerShell:
.\elite\deploy_elite.ps1 -Ip 192.168.2.1 -DebugBusybox
```

Or copy manually:
1. Build or locate the pre-compiled ARM binary:
   ```bash
   ./build/compile_all.sh elite
   ```
   *(Pre-built static binary resides at `elite/bin/codex_elite`)*.

2. Copy the binary to `/data/`:
   ```bash
   scp elite/bin/codex_elite root@192.168.2.1:/data/codex_elite
   ssh root@192.168.2.1 "chmod +x /data/codex_elite"
   # Optional: Only copy extended BusyBox if debugging
   # scp elite/bin/busybox root@192.168.2.1:/data/busybox
   ```

3. Update `/data/init.sh` to stop the stock monolithic Lua UI and launch HarmonyNG (`codex_elite`):
   ```bash
   cat << 'EOF' > /data/init.sh
   #!/bin/sh
   # Terminate stock Lua UI to release framebuffer and /dev/rfspi
   killall -9 luadraws 2>/dev/null || true
   
   # Start dropbear SSH daemon
   if ! pidof dropbear >/dev/null; then
       /usr/sbin/dropbear -p 22 -B
   fi

   # Start HarmonyNG custom firmware
   /data/codex_elite >/dev/null 2>&1 &
   EOF
   chmod +x /data/init.sh
   ```

4. Launch HarmonyNG manually for the first run:
   ```bash
   ssh root@192.168.2.1
   killall -9 luadraws 2>/dev/null || true
   /data/codex_elite &
   ```

---

## 6. Battery Life & Power Optimization Settings

The custom `codex_elite` firmware includes power management settings to extend battery life:

1. **Button Backlight Control**:
   - Navigate to the **Settings** tab on the Elite touch screen.
   - Tap **Button Backlight**:
     - `OFF`: Always keeps physical keypad LEDs off (maximum battery savings).
     - `ON`: Always illuminates keypads on remote pickup or button press.
     - `NIGHT`: Automatically turns off button backlights between **22:00 and 07:00** (configurable window) while keeping screen backlight active when touched.
2. **Gyroscope Poll Rate**:
   - Stock Harmony firmware queries the LIS3DH I2C accelerometer at 50 Hz, constantly waking the DaVinci CPU.
   - Tap **Gyro Poll Rate** in Settings to select:
     - `10 Hz` (**Recommended**): Drops pickup power consumption by over 60% while detecting remote lift gestures.
     - `25 Hz`: Balanced response.
     - `50 Hz`: High sensitivity.
3. Settings are saved to `/data/codex_elite.conf`.

---

## 7. RF Pairing with Harmony Hub

1. On the Elite remote touch screen, open the **Settings** dock.
2. Select **RF Pairing Menu** -> **Start Pairing to Hub**.
3. In the Hub Web console (`http://<YOUR_HUB_IP>:8089`), go to **Harmony Elite Remote** and click **Start Hub Pairing Mode**.
4. Within 5 seconds, the Hub and remote exchange RF tokens, the top badge shows a teal link, and current activities and button maps sync automatically.

---

## 8. Factory Reset & Reverting to Stock Firmware

To completely remove HarmonyNG software and revert the Elite remote (or Hub) to official Logitech stock firmware:

### Using the Dedicated Factory Reset Tool
```bash
# Revert Elite Remote:
python tools/factory_reset.py --target elite --elite-ip 192.168.2.1

# Revert Harmony Hub:
python tools/factory_reset.py --target hub --hub-ip <HUB_IP>

# Revert both Hub and Remote:
python tools/factory_reset.py --target all
```

### Using Deployment Scripts
```bash
# PowerShell:
.\elite\deploy_elite.ps1 -Ip 192.168.2.1 -FactoryReset
.\deploy_hub.ps1 -HubIp <HUB_IP> -FactoryReset

# Python:
python elite/deploy_elite.py --ip 192.168.2.1 --factory-reset
```

### What Factory Reset Does:
1. **Restores original startup scripts**: Replaces `/etc/init.d/rcS.local` with the stock backup (`/data/stock_backup/rcS.local.orig`) or removes it if stock had none.
2. **Restores stock daemon execution permissions**: Runs `chmod +x` on `/usr/bin/watchdog`, `/usr/bin/usbhid`, `/usr/bin/hal`, `/opt/luaworks/luaworks`, and `/opt/luadraws/luadraws`.
3. **Purges all custom binaries and configs**: Deletes `/data/codex_*`, `/data/dropbear*`, `/etc/nowatchdog`, `/data/reboot_counter`, and Dropbear symlinks.
4. **Reboots cleanly** into official Logitech stock software.

